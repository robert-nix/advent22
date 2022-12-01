#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#define INPUT_BUFFER_SIZE (4 * 1024 * 1024)
static char input_buffer[INPUT_BUFFER_SIZE + 1];
static size_t input_buffer_cursor;

static size_t input_write_callback(char *ptr, size_t size, size_t nmemb,
                                   void *userdata) {
  size_t bytes = size * nmemb;
  if (input_buffer_cursor + bytes > INPUT_BUFFER_SIZE) {
    fprintf(stderr, "error: input buffer length exceeded");
    bytes = INPUT_BUFFER_SIZE - input_buffer_cursor;
  }
  if (bytes > 0) {
    memcpy(input_buffer + input_buffer_cursor, ptr, bytes);
    input_buffer_cursor += bytes;
  }
  return bytes;
}

struct pipeline_node;

typedef void (*pipeline_fn)(struct pipeline_node *next, void *userdata,
                            void *item);

#define pipeline_end ((void *)(intptr_t)-1)

typedef struct pipeline_node {
  struct pipeline_node *next;
  void *userdata;
  pipeline_fn fn;
} pipeline_node_t;

typedef struct {
  char *input;
  pipeline_node_t *head;
} pipeline_t;

typedef struct {
  char buf[16];
  size_t buf_pos;
  char delim;
} pipe_group_state_t;

static void pipe_group(pipeline_node_t *next, void *userdata, void *item) {
  pipe_group_state_t *state = (pipe_group_state_t *)userdata;
  if (item == pipeline_end) {
    state->buf[state->buf_pos] = '\0';
    next->fn(next->next, next->userdata, (void *)&state->buf);
    next->fn(next->next, next->userdata, pipeline_end);
    return;
  }
  char c = *(char *)item;
  if (c == state->delim) {
    state->buf[state->buf_pos] = '\0';
    state->buf_pos = 0;
    next->fn(next->next, next->userdata, (void *)&state->buf);
  } else {
    state->buf[state->buf_pos++] = c;
  }
}

static void pipe_str_to_optional_int(pipeline_node_t *next, void *userdata,
                                     void *item) {
  if (item == pipeline_end) {
    next->fn(next->next, next->userdata, pipeline_end);
    return;
  }
  char *str = (char *)item;
  int i = 0;
  if (!*str)
    goto empty;
  while (*str) {
    if (*str < '0' || *str > '9')
      goto empty;
    i = i * 10 + (*str - '0');
    str++;
  }
  next->fn(next->next, next->userdata, (void *)&i);
  return;
empty:
  next->fn(next->next, next->userdata, NULL);
}

static void pipe_delimsum_optional_ints(pipeline_node_t *next, void *userdata,
                                        void *item) {
  int *sum = (int *)userdata;
  int *i = (int *)item;
  if (item == pipeline_end) {
    next->fn(next->next, next->userdata, (void *)sum);
    next->fn(next->next, next->userdata, pipeline_end);
  } else if (i) {
    *sum += *i;
  } else {
    next->fn(next->next, next->userdata, (void *)sum);
    *sum = 0;
  }
}

static void pipe_max_optional_int(pipeline_node_t *next, void *userdata,
                                  void *item) {
  int *max = (int *)userdata;
  if (item == pipeline_end) {
    next->fn(next->next, next->userdata, (void *)max);
    next->fn(next->next, next->userdata, pipeline_end);
    return;
  } else if (!item) {
    return;
  }
  int i = *(int *)item;
  if (i > *max) {
    *max = i;
  }
}

typedef struct {
  pipeline_node_t *children;
  int n;
} pipe_fanout_t;

static void pipe_fanout(pipeline_node_t *next, void *userdata, void *item) {
  pipe_fanout_t *fanout = (pipe_fanout_t *)userdata;
  assert(!next);
  for (int i = 0; i < fanout->n; i++) {
    fanout->children[i].fn(&fanout->children[i], fanout->children[i].userdata,
                           item);
  }
}

typedef struct {
  int *arr;
  int n;
} int_array_t;

static void pipe_topn_optional_ints(pipeline_node_t *next, void *userdata,
                                    void *item) {
  int_array_t *state = (int_array_t *)userdata;
  if (item == pipeline_end) {
    next->fn(next->next, next->userdata, (void *)state);
    next->fn(next->next, next->userdata, pipeline_end);
    return;
  } else if (!item) {
    return;
  }
  int i = *(int *)item;
  for (int j = 0; j < state->n; j++) {
    if (i > state->arr[j]) {
      for (int k = state->n - 1; k > j; k--) {
        state->arr[k] = state->arr[k - 1];
      }
      state->arr[j] = i;
      break;
    }
  }
}

static void pipe_sum_int_array(pipeline_node_t *next, void *userdata,
                               void *item) {
  if (item == pipeline_end) {
    next->fn(next->next, next->userdata, pipeline_end);
  } else {
    int_array_t *array = (int_array_t *)item;
    int sum = 0;
    for (int i = 0; i < array->n; i++) {
      sum += array->arr[i];
    }
    next->fn(next->next, next->userdata, (void *)&sum);
  }
}

static void pipe_out_str(pipeline_node_t *next, void *userdata, void *item) {
  printf("%s\n", (char *)item);
}

static void pipe_out_optional_int(pipeline_node_t *next, void *userdata,
                                  void *item) {
  if (item == pipeline_end)
    return;
  if (item) {
    if (userdata) {
      printf("%s%d\n", (char *)userdata, *(int *)item);
    } else {
      printf("%d\n", *(int *)item);
    }
  } else {
    puts(userdata ? (char *)userdata : "");
  }
}

static void pipeline_process(pipeline_t *pipeline) {
  pipeline_node_t *node = pipeline->head;
  char *input = pipeline->input;
  while (*input) {
    node->fn(node->next, node->userdata, (void *)input);
    input++;
  }
  node->fn(node->next, node->userdata, pipeline_end);
}

static int day1(char *input) {
  pipeline_t pipeline = {.input = input};

  pipe_group_state_t group_state = {.delim = '\n', .buf_pos = 0};
  pipeline_node_t group = {.userdata = (void *)&group_state, .fn = pipe_group};

  pipeline_node_t to_int = {.fn = pipe_str_to_optional_int};
  group.next = &to_int;

  int sum = 0;
  pipeline_node_t sum_ints = {.userdata = (void *)&sum,
                              .fn = pipe_delimsum_optional_ints};
  to_int.next = &sum_ints;

  pipeline_node_t parts[2];
  pipe_fanout_t fanout = {.children = parts, .n = 2};
  pipeline_node_t fanout_node = {.userdata = (void *)&fanout,
                                 .fn = pipe_fanout};
  sum_ints.next = &fanout_node;

  int max = 0;
  parts[0].userdata = (void *)&max;
  parts[0].fn = pipe_max_optional_int;

  int topn[3];
  int_array_t topn_state = {.arr = topn, .n = 3};
  parts[1].userdata = (void *)&topn_state;
  parts[1].fn = pipe_topn_optional_ints;

  pipeline_node_t out_part_one = {.fn = pipe_out_optional_int,
                                  .userdata = "Part 1: "};
  parts[0].next = &out_part_one;

  pipeline_node_t int_array_sum = {.fn = pipe_sum_int_array};
  parts[1].next = &int_array_sum;

  pipeline_node_t out_part_two = {.fn = pipe_out_optional_int,
                                  .userdata = "Part 2: "};
  int_array_sum.next = &out_part_two;

  pipeline.head = &group;
  pipeline_process(&pipeline);

  return 0;
}

// array of function pointers for each day's executor
typedef int (*day_executor)(char *);
static const day_executor day_executors[] = {
    day1,
};

int main(int argc, char **argv) {
  int day = 0;

  if (argc > 1) {
    day = atoi(argv[1]);
  }

  if (day < 1 || day > (sizeof(day_executors) / sizeof(day_executor))) {
    fprintf(stderr, "Usage: %s <day [1-%lu]>\n", argv[0],
            sizeof(day_executors) / sizeof(day_executor));
    return 64;
  }

  char *session = getenv("ADVENT_SESSION");
  if (session == NULL) {
    fputs("ADVENT_SESSION environment variable not set\n", stderr);
    return 78;
  }

  char url[128];
  if (!snprintf(url, sizeof(url), "https://adventofcode.com/2022/day/%d/input",
                day)) {
    __builtin_trap();
    return 1;
  }

  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    __builtin_trap();
    return 1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_COOKIE, session);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, input_write_callback);
  CURLcode result = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    fprintf(stderr, "curl_easy_perform() failed: %s",
            curl_easy_strerror(result));
    return 1;
  }

  input_buffer[input_buffer_cursor] = '\0';

  return day_executors[day - 1](input_buffer);
}
