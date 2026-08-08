#include "kernel/types.h"
#include "kernel/shmem.h"
#include "user/user.h"

#define TIMEBASE_HZ 10000000ULL
#define TIMEOUT_MS 2000
#define WARMUP_ITERS 20
#define RTT_ITERS 200
#define BREAKDOWN_ITERS 200
#define THROUGHPUT_ITERS 1000
#define THROUGHPUT_PAYLOAD 64

static uint64 samples[THROUGHPUT_ITERS];
static uint64 forward_samples[BREAKDOWN_ITERS];
static uint64 handler_samples[BREAKDOWN_ITERS];
static uint64 return_samples[BREAKDOWN_ITERS];
static char req[SHMSG_PAYLOAD_SIZE];
static char reply[SHMSG_PAYLOAD_SIZE];

static void
fill_payload(char *buf, int len)
{
  for(int i = 0; i < len; i++)
    buf[i] = 'a' + (i % 26);
}

static uint64
ticks_to_us(uint64 ticks)
{
  return (ticks * 1000000ULL) / TIMEBASE_HZ;
}

static uint64
read_le64(char *p)
{
  uint64 v = 0;

  for(int i = 0; i < 8; i++)
    v |= ((uint64)(uchar)p[i]) << (8 * i);
  return v;
}

static void
sort(uint64 *v, int n)
{
  for(int i = 1; i < n; i++){
    uint64 x = v[i];
    int j = i - 1;

    while(j >= 0 && v[j] > x){
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
}

static void
print_stats(char *name, uint64 *v, int n)
{
  uint64 sum = 0;

  sort(v, n);
  for(int i = 0; i < n; i++)
    sum += v[i];

  printf("  %s: min=%luus  max=%luus  avg=%luus  p50=%luus  p99=%luus\n",
         name,
         ticks_to_us(v[0]),
         ticks_to_us(v[n - 1]),
         ticks_to_us(sum / n),
         ticks_to_us(v[n / 2]),
         ticks_to_us(v[(n * 99) / 100]));
}

static int
rpc_once(int ep, int payload_len, int *reply_len, uint64 *start, uint64 *end)
{
  int n;

  *start = rdtime();
  n = rpccall(ep, SHMEM_CMD_BENCH, req, payload_len,
              reply, SHMSG_PAYLOAD_SIZE, TIMEOUT_MS);
  *end = rdtime();
  if(n < 0){
    printf("benchtest: rpccall failed ep=0x%x payload=%d\n",
           ep, payload_len);
    return -1;
  }

  *reply_len = n;
  return 0;
}

static int
test_rtt(void)
{
  int n;
  uint64 t0;
  uint64 t1;

  printf("\n--- Test 1: RPC Round-Trip Time ---\n");
  fill_payload(req, 16);

  for(int i = 0; i < WARMUP_ITERS; i++){
    if(rpc_once(SHMEM_EP_RTOS_ECHO, 16, &n, &t0, &t1) < 0)
      return -1;
  }

  for(int i = 0; i < RTT_ITERS; i++){
    if(rpc_once(SHMEM_EP_RTOS_ECHO, 16, &n, &t0, &t1) < 0)
      return -1;
    samples[i] = t1 - t0;
  }

  print_stats("rpc_rtt", samples, RTT_ITERS);
  printf("  jitter: max-min=%luus  p99-p50=%luus\n",
         ticks_to_us(samples[RTT_ITERS - 1] - samples[0]),
         ticks_to_us(samples[(RTT_ITERS * 99) / 100] -
                     samples[RTT_ITERS / 2]));
  return 0;
}

static int
test_breakdown(void)
{
  int n;
  uint64 t0;
  uint64 t1;

  printf("\n--- Test 2: Latency Breakdown ---\n");
  fill_payload(req, 32);

  for(int i = 0; i < WARMUP_ITERS; i++){
    if(rpc_once(SHMEM_EP_RTOS_BENCH, 32, &n, &t0, &t1) < 0)
      return -1;
  }

  for(int i = 0; i < BREAKDOWN_ITERS; i++){
    uint64 t_rx;
    uint64 t_tx;

    if(rpc_once(SHMEM_EP_RTOS_BENCH, 32, &n, &t0, &t1) < 0)
      return -1;
    if(n < 16){
      printf("benchtest: short bench reply len=%d\n", n);
      return -1;
    }

    t_rx = read_le64(reply);
    t_tx = read_le64(reply + 8);
    forward_samples[i] = t_rx >= t0 ? t_rx - t0 : 0;
    handler_samples[i] = t_tx >= t_rx ? t_tx - t_rx : 0;
    return_samples[i] = t1 >= t_tx ? t1 - t_tx : 0;
  }

  print_stats("forward_latency", forward_samples, BREAKDOWN_ITERS);
  print_stats("handler_time", handler_samples, BREAKDOWN_ITERS);
  print_stats("return_latency", return_samples, BREAKDOWN_ITERS);
  return 0;
}

static int
test_throughput(void)
{
  int n;
  uint64 t0;
  uint64 t1;
  uint64 elapsed;
  uint64 msg_per_sec;
  uint64 kb_per_sec;

  printf("\n--- Test 3: Ping-Pong Throughput ---\n");
  fill_payload(req, THROUGHPUT_PAYLOAD);

  t0 = rdtime();
  for(int i = 0; i < THROUGHPUT_ITERS; i++){
    n = rpccall(SHMEM_EP_RTOS_BENCH, SHMEM_CMD_BENCH, req,
                THROUGHPUT_PAYLOAD, reply, SHMSG_PAYLOAD_SIZE, TIMEOUT_MS);
    if(n < 0){
      printf("benchtest: throughput failed iter=%d\n", i);
      return -1;
    }
  }
  t1 = rdtime();

  elapsed = t1 - t0;
  msg_per_sec = ((uint64)THROUGHPUT_ITERS * TIMEBASE_HZ) / elapsed;
  kb_per_sec = ((uint64)THROUGHPUT_ITERS * THROUGHPUT_PAYLOAD *
                TIMEBASE_HZ) / elapsed / 1024;

  printf("  ping-pong: %d msgs (%dB each) in %luus\n",
         THROUGHPUT_ITERS, THROUGHPUT_PAYLOAD, ticks_to_us(elapsed));
  printf("  rate=%lu msg/s  bandwidth=%lu KB/s\n",
         msg_per_sec, kb_per_sec);
  return 0;
}

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  printf("=== ICC Performance Benchmark (Stage 8) ===\n");
  printf("timer_freq=%luHz, rtt_iters=%d, bp_iters=%d, tp_iters=%d, tp_payload=%dB\n",
         (uint64)TIMEBASE_HZ, RTT_ITERS, BREAKDOWN_ITERS,
         THROUGHPUT_ITERS, THROUGHPUT_PAYLOAD);

  if(test_rtt() < 0)
    exit(1);
  if(test_breakdown() < 0)
    exit(1);
  if(test_throughput() < 0)
    exit(1);

  printf("\n=== Benchmark Complete ===\n");
  exit(0);
}
