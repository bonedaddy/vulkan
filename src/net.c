// Copyright (c) 2019, The Vulkan Developers.
//
// This file is part of Vulkan.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// You should have received a copy of the MIT License
// along with Vulkan. If not, see <https://opensource.org/licenses/MIT>.

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>

#include <gossip.h>
#include <config.h>

#include "chainparams.h"
#include "task.h"
#include "protocol.h"

#include "net.h"

static int g_net_server_running = 0;
static int g_net_seed_mode = 0;

static pittacus_gossip_t *g_net_gossip = NULL;
static task_t *g_net_resync_chain_task = NULL;

void net_set_gossip(pittacus_gossip_t *gossip)
{
  g_net_gossip = gossip;
}

pittacus_gossip_t* net_get_gossip(void)
{
  return g_net_gossip;
}

void net_receive_data(void *context, pittacus_gossip_t *gossip, const uint8_t *data, size_t data_size)
{
  if (handle_receive_packet(gossip, data, data_size))
  {
    fprintf(stderr, "Failed to handling incoming packet\n");
    return;
  }
}

void net_send_data(pittacus_gossip_t *gossip, const uint8_t *data, size_t data_size)
{
  pittacus_gossip_send_data(gossip, data, data_size);
}

int net_connect(const char *address, int port)
{
  struct sockaddr_in self_in;
  self_in.sin_family = AF_INET;
  self_in.sin_port = 0;
  inet_aton("0.0.0.0", &self_in.sin_addr);

  pittacus_addr_t self_addr = {
    .addr = (const pt_sockaddr *) &self_in,
    .addr_len = sizeof(struct sockaddr_in)
  };

  pittacus_gossip_t *gossip = pittacus_gossip_create(&self_addr, &net_receive_data, NULL);
  if (gossip == NULL)
  {
    fprintf(stderr, "Gossip initialization failed: %s\n", strerror(errno));
    return 1;
  }

  struct sockaddr_in seed_node_in;
  seed_node_in.sin_family = AF_INET;
  seed_node_in.sin_port = htons(port);
  inet_aton(address, &seed_node_in.sin_addr);

  pittacus_addr_t seed_node_addr = {
    .addr = (const pt_sockaddr *) &seed_node_in,
    .addr_len = sizeof(struct sockaddr_in)
  };

  int join_result = pittacus_gossip_join(gossip, &seed_node_addr, 1);
  if (join_result < 0)
  {
    fprintf(stderr, "Gossip join failed: %d\n", join_result);
    pittacus_gossip_destroy(gossip);
    return 1;
  }

  g_net_gossip = gossip;
  return 0;
}

int net_open_connection(void)
{
  struct sockaddr_in self_in;
  self_in.sin_family = AF_INET;
  self_in.sin_port = htons(P2P_PORT);
  inet_aton("0.0.0.0", &self_in.sin_addr);

  pittacus_addr_t self_addr = {
    .addr = (const pt_sockaddr *) &self_in,
    .addr_len = sizeof(struct sockaddr_in)
  };

  pittacus_gossip_t *gossip = pittacus_gossip_create(&self_addr, &net_receive_data, NULL);
  if (gossip == NULL)
  {
    fprintf(stderr, "Gossip initialization failed: %s\n", strerror(errno));
    return 1;
  }

  int join_result = pittacus_gossip_join(gossip, NULL, 0);
  if (join_result < 0)
  {
    fprintf(stderr, "Gossip join failed: %d\n", join_result);
    pittacus_gossip_destroy(gossip);
    return 1;
  }

  g_net_gossip = gossip;
  return 0;
}

int net_run_server(void)
{
  int is_seed_node = g_net_seed_mode || NUM_SEED_NODES == 0;
  if (is_seed_node)
  {
    if (net_open_connection())
    {
      fprintf(stderr, "Failed to open seed node connection!");
      return 1;
    }
  }
  else
  {
    for (int i = 0; i < NUM_SEED_NODES; i++)
    {
      seed_node_entry_t seed_node_entry = SEED_NODES[i];
      if (net_connect(seed_node_entry.address, seed_node_entry.port))
      {
        fprintf(stderr, "Failed to connect to seed with address: %s:%d!\n", seed_node_entry.address, seed_node_entry.port);
        return 1;
      }
      else
      {
        break;
      }
    }
  }

  pt_socket_fd gossip_fd = pittacus_gossip_socket_fd(g_net_gossip);
  struct pollfd gossip_poll_fd = {
    .fd = gossip_fd,
    .events = POLLIN,
    .revents = 0
  };

  int poll_interval = GOSSIP_TICK_INTERVAL;
  int recv_result = 0;
  int send_result = 0;
  int poll_result = 0;
  time_t previous_data_msg_ts = time(NULL);

  while (g_net_server_running)
  {
    gossip_poll_fd.revents = 0;
    poll_result = poll(&gossip_poll_fd, 1, poll_interval);
    if (poll_result > 0)
    {
      if (gossip_poll_fd.revents & POLLERR)
      {
        fprintf(stderr, "Gossip socket failure: %s\n", strerror(errno));
        pittacus_gossip_destroy(g_net_gossip);
        return 1;
      }
      else if (gossip_poll_fd.revents & POLLIN)
      {
        recv_result = pittacus_gossip_process_receive(g_net_gossip);
        if (recv_result < 0)
        {
          fprintf(stderr, "Gossip receive failed: %d\n", recv_result);
          pittacus_gossip_destroy(g_net_gossip);
          return 1;
        }
      }
    }
    else if (poll_result < 0)
    {
      fprintf(stderr, "Poll failed: %s\n", strerror(errno));
      pittacus_gossip_destroy(g_net_gossip);
      return 1;
    }

    poll_interval = pittacus_gossip_tick(g_net_gossip);
    if (poll_interval < 0)
    {
      fprintf(stderr, "Gossip tick failed: %d\n", poll_interval);
      return 1;
    }

    // update the task manager
    taskmgr_tick();

    send_result = pittacus_gossip_process_send(g_net_gossip);
    if (send_result < 0)
    {
      fprintf(stderr, "Gossip send failed: %d, %s\n", send_result, strerror(errno));
      pittacus_gossip_destroy(g_net_gossip);
      return 1;
    }
  }

  pittacus_gossip_destroy(g_net_gossip);
  return 0;
}

void* net_run_server_threaded()
{
  net_run_server();
  return NULL;
}

int net_start_server(int threaded, int seed_mode)
{
  if(g_net_server_running)
  {
    return 1;
  }

  g_net_server_running = 1;
  g_net_seed_mode = seed_mode;

  g_net_resync_chain_task = add_task(resync_chain, RESYNC_CHAIN_TASK_DELAY);

  if (threaded)
  {
    pthread_t thread;
    pthread_create(&thread, NULL, net_run_server_threaded, NULL);
    return 0;
  }
  else
  {
    return net_run_server();
  }
}

void net_stop_server(void)
{
  if(!g_net_server_running)
  {
    return;
  }
  g_net_server_running = 0;
}

task_result_t resync_chain(task_t *task, va_list args)
{
  handle_broadcast_packet(PKT_TYPE_GET_BLOCK_HEIGHT_REQ);
  return TASK_RESULT_WAIT;
}
