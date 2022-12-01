#include <assert.h>
#include <ctype.h>
#include <errno.h>
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

typedef void (*pipeline_stage_init_fn)(struct pipeline_node *node, void *arg);

typedef void (*pipeline_stage_free_fn)(struct pipeline_node *node);

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

static void pipe_group_init(pipeline_node_t *node, void *arg) {
  node->userdata = malloc(sizeof(pipe_group_state_t));
  pipe_group_state_t *state = (pipe_group_state_t *)node->userdata;
  state->buf_pos = 0;
  state->delim = (char)(intptr_t)arg;
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

static void pipe_delimsum_optional_ints_init(struct pipeline_node *node,
                                             void *arg) {
  int *sum = malloc(sizeof(int));
  *sum = 0;
  node->userdata = sum;
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

static void pipe_max_optional_int_init(struct pipeline_node *node, void *arg) {
  int *max = malloc(sizeof(int));
  *max = 0;
  node->userdata = max;
}

typedef struct {
  pipeline_node_t *children;
  int n;
} pipe_fanout_t;

static void pipe_fanout(pipeline_node_t *next, void *userdata, void *item) {
  pipe_fanout_t *fanout = (pipe_fanout_t *)userdata;
  assert(!next);
  for (int i = 0; i < fanout->n; i++) {
    pipeline_node_t *child = &fanout->children[i];
    child->fn(child->next, child->userdata, item);
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

static void pipe_topn_optional_ints_init(struct pipeline_node *node,
                                         void *arg) {
  int_array_t *state = malloc(sizeof(int_array_t));
  int n = (int)(intptr_t)arg;
  state->n = n;
  state->arr = malloc(sizeof(int) * n);
  for (int i = 0; i < n; i++) {
    state->arr[i] = 0;
  }
  node->userdata = state;
}

static void pipe_topn_options_ints_free(struct pipeline_node *node) {
  int_array_t *state = (int_array_t *)node->userdata;
  free(state->arr);
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

static void pipe_out_optional_int_init(struct pipeline_node *node, void *arg) {
  node->userdata = arg;
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

typedef enum {
  TY_NONE,
  // values of integral types are pointers-to-values within stage functions
  TY_CHAR,
  TY_STR,
  TY_INT,
  TY_INT_ARRAY,
} item_type_t;

static const char *item_type_str(item_type_t ty) {
  switch (ty) {
  case TY_NONE:
    return "none";
  case TY_CHAR:
    return "char";
  case TY_STR:
    return "str";
  case TY_INT:
    return "int";
  case TY_INT_ARRAY:
    return "int_array";
  default:
    return "unknown";
  }
}

typedef struct {
  const char *name;
  pipeline_fn fn;
  pipeline_stage_init_fn init;
  pipeline_stage_free_fn free; // for any mallocs other than userdata
  int input_type;
  int output_type;
  int arg_type;
} pipeline_stage_t;

static pipeline_stage_t stages[] = {
    {"group", pipe_group, pipe_group_init, NULL, TY_CHAR, TY_STR, TY_CHAR},
    {"to_optional_int", pipe_str_to_optional_int, NULL, NULL, TY_STR, TY_INT,
     TY_NONE},
    {"delimsum", pipe_delimsum_optional_ints, pipe_delimsum_optional_ints_init,
     NULL, TY_INT, TY_INT, TY_NONE},
    {"max", pipe_max_optional_int, pipe_max_optional_int_init, NULL, TY_INT,
     TY_INT, TY_NONE},
    {"topn", pipe_topn_optional_ints, pipe_topn_optional_ints_init,
     pipe_topn_options_ints_free, TY_INT, TY_INT_ARRAY, TY_INT},
    {"sum", pipe_sum_int_array, NULL, NULL, TY_INT_ARRAY, TY_INT, TY_NONE},
    {"print", pipe_out_str, NULL, NULL, TY_STR, TY_NONE, TY_NONE},
    {"print", pipe_out_optional_int, pipe_out_optional_int_init, NULL, TY_INT,
     TY_NONE, TY_STR},
};

static void tok_ident(char **s) {
  while (isalpha(**s) || isdigit(**s) || **s == '_') {
    (*s)++;
  }
}

static pipeline_t *parse_pipeline(char *program) {
  // syntax:
  // ident |> stage [|> ...] [-> ident]
  // stages may take literal args, such as group('\n') or topn(3)
  int state = 0;
  pipeline_t *pipeline = malloc(sizeof(pipeline_t));
  char *idents[8];
  pipe_fanout_t *fanouts[8];
  int ident_types[8];
  int n_idents = 0;
  pipeline->head = malloc(sizeof(pipeline_node_t));
  pipeline_node_t *curr_head = pipeline->head;
  int stage_id = -1;
  int line = 1;
  int curr_type = TY_CHAR;
  char *line_start = program;
  while (*program) {
    char c = *program;
    if (isspace(c)) {
      program++;
      if (c == '\n') {
        line++;
        line_start = program;
      }
      continue;
    }
    switch (state) {
    case 0: // pipeline head (ident or "input")
      if (isalpha(c)) {
        char *ident = program;
        tok_ident(&program);
        if (!strncmp(ident, "input", program - ident)) {
          if (curr_head != pipeline->head) {
            fprintf(stderr, "input as fanout is unsupported\n");
            goto error;
          }
        } else {
          // find fanout or error
          pipe_fanout_t *fanout = NULL;
          int fanout_i = 0;
          for (int i = 0; i < n_idents; i++) {
            if (!strncmp(ident, idents[i], program - ident)) {
              fanout = fanouts[i];
              fanout_i = i;
              break;
            }
          }
          if (!fanout) {
            fprintf(stderr, "unknown ident '%.*s'", (int)(program - ident),
                    ident);
            goto error;
          }
          if (fanout->n == 8) {
            fprintf(stderr, "fanout '%.*s' has too many children\n",
                    (int)(program - ident), ident);
            goto error;
          }
          curr_head = &fanout->children[fanout->n++];
          curr_type = ident_types[fanout_i];
        }
        state = 1;
      } else {
        fprintf(stderr, "expected start of identifer, got '%c'\n", c);
        goto error;
      }
      break;
    case 1: // expect "|>" or "->"
      if (c == '|') {
        program++;
        if (*program != '>') {
          fprintf(stderr, "expected '|>', got '|%c'\n", *program);
          goto error;
        }
        program++;
        state = 2;
      } else if (c == '-') {
        program++;
        if (*program != '>') {
          fprintf(stderr, "expected '->', got '-%c'\n", *program);
          goto error;
        }
        program++;
        state = 3;
      } else {
        fprintf(stderr, "expected '|>', got '%c'\n", c);
        goto error;
      }
      break;
    case 2: // expect stage
      if (isalpha(c)) {
        char *stage = program;
        tok_ident(&program);
        stage_id = -1;
        for (int i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
          if (!strncmp(stage, stages[i].name, program - stage)) {
            if (stages[i].input_type == curr_type) {
              stage_id = i;
              break;
            }
          }
        }
        if (stage_id == -1) {
          fprintf(stderr, "unknown stage: %.*s<%s>\n", (int)(program - stage),
                  stage, item_type_str(curr_type));
          goto error;
        }
        curr_head->fn = stages[stage_id].fn;
        curr_head->userdata = NULL;
        curr_type = stages[stage_id].output_type;
        if (stages[stage_id].arg_type != TY_NONE) {
          state = 4;
        } else {
          if (stages[stage_id].init != NULL) {
            stages[stage_id].init(curr_head, NULL);
          }
          if (curr_type == TY_NONE) {
            curr_head->next = NULL;
            curr_head = NULL;
            state = 0;
          } else {
            state = 1;
            curr_head->next = malloc(sizeof(pipeline_node_t));
            curr_head = curr_head->next;
          }
        }
      } else {
        fprintf(stderr, "expected start of stage, got '%c'\n", c);
        goto error;
      }
      break;
    case 3: // expect ident for fanout
      if (isalpha(c)) {
        char *ident = program;
        tok_ident(&program);
        if (!strncmp(ident, "input", program - ident)) {
          fprintf(stderr, "cannot use 'input' as fanout target\n");
          goto error;
        }
        if (n_idents == 8) {
          fprintf(stderr, "too many fanouts\n");
          goto error;
        }
        idents[n_idents] = strndup(ident, program - ident);
        fanouts[n_idents] = malloc(sizeof(pipe_fanout_t));
        pipe_fanout_t *fanout = fanouts[n_idents];
        fanout->n = 0;
        fanout->children = malloc(sizeof(pipeline_node_t) * 8);
        curr_head->fn = pipe_fanout;
        curr_head->userdata = fanout;
        curr_head->next = NULL;
        curr_head = NULL;
        ident_types[n_idents] = curr_type;
        n_idents++;
        state = 0;
      } else {
        fprintf(stderr, "expected start of identifer, got '%c'\n", c);
        goto error;
      }
      break;
    case 4: // expect ( and literal arg
      if (c == '(') {
        program++;
        state = 5;
      } else {
        fprintf(stderr, "expected '(', got '%c'\n", c);
        goto error;
      }
      break;
    case 5: // expect literal arg
      if (isdigit(c)) {
        char *arg = program;
        int arg_val = strtol(arg, &program, 10);
        if (stages[stage_id].arg_type == TY_INT) {
          stages[stage_id].init(curr_head, (void *)(intptr_t)arg_val);
        } else {
          fprintf(stderr, "unexpected literal arg type\n");
          goto error;
        }
        state = 6;
      } else if (c == '\'') {
        program++;
        char *arg = program;
        while (*program != '\'')
          program++;
        char arg_val = *arg;
        if (arg_val == '\\') {
          if (arg[1] == 'n')
            arg_val = '\n';
        }
        if (stages[stage_id].arg_type == TY_CHAR) {
          stages[stage_id].init(curr_head, (void *)(intptr_t)arg_val);
        } else {
          fprintf(stderr, "unexpected literal arg type\n");
          goto error;
        }
        program++;
        state = 6;
      } else if (c == '"') {
        program++;
        char *arg = program;
        while (*program != '"')
          program++;
        char *arg_val = strndup(arg, program - arg);
        if (stages[stage_id].arg_type == TY_STR) {
          stages[stage_id].init(curr_head, (void *)arg_val);
        } else {
          fprintf(stderr, "unexpected literal arg type\n");
          goto error;
        }
        program++;
        state = 6;
      } else {
        fprintf(stderr, "expected literal arg, got '%c'\n", c);
        goto error;
      }
      break;
    case 6: // expect )
      if (c == ')') {
        program++;
        if (curr_type == TY_NONE) {
          curr_head->next = NULL;
          curr_head = NULL;
          state = 0;
        } else {
          curr_head->next = malloc(sizeof(pipeline_node_t));
          curr_head = curr_head->next;
          state = 1;
        }
      } else {
        fprintf(stderr, "expected ')', got '%c'\n", c);
        goto error;
      }
      break;
    }
  }

  if (state != 0) {
    fprintf(stderr, "unexpected end of program\n");
    goto error;
  }

  for (int i = 0; i < n_idents; i++) {
    free(idents[i]);
  }
  return pipeline;

error:
  fprintf(stderr, "  at %d:%d\n", line, (int)(program - line_start + 1));

  for (int i = 0; i < n_idents; i++) {
    free(idents[i]);
  }
  free(pipeline);
  return NULL;
}

static void debug_print_pipeline(pipeline_node_t *pipeline, int indent) {
  for (int i = 0; i < indent; i++)
    printf(" ");
  if ((intptr_t)pipeline == (intptr_t)0xbebebebebebebebeull) {
    printf("!!! uninitialized memory reading pipeline\n");
    return;
  }
  for (int i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
    if (pipeline->fn == stages[i].fn) {
      printf("%s", stages[i].name);
      break;
    }
  }
  if (pipeline->next != NULL) {
    printf(" ->\n");
    debug_print_pipeline(pipeline->next, indent + 2);
  } else if (pipeline->fn == pipe_fanout) {
    pipe_fanout_t *fanout = pipeline->userdata;
    printf("fanout (n: %d):\n", fanout->n);
    for (int i = 0; i < fanout->n; i++) {
      debug_print_pipeline(&fanout->children[i], indent + 2);
    }
    printf("\n");
  } else {
    printf("\n");
  }
}

static void pipeline_free_node(pipeline_node_t *node, int free_node) {
  if (node->fn == pipe_fanout) {
    pipe_fanout_t *fanout = node->userdata;
    for (int i = 0; i < fanout->n; i++) {
      pipeline_free_node(&fanout->children[i], 0);
    }
    free(fanout->children);
    free(fanout);
  } else {
    for (int i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
      if (node->fn == stages[i].fn) {
        if (stages[i].free != NULL)
          stages[i].free(node);
        break;
      }
    }
    if (node->userdata != NULL) {
      free(node->userdata);
    }
    if (node->next != NULL) {
      pipeline_free_node(node->next, 1);
    }
  }
  if (free_node)
    free(node);
}

static void pipeline_free(pipeline_t *pipeline) {
  pipeline_free_node(pipeline->head, 1);
  free(pipeline);
}

int main(int argc, const char **argv) {
  int debug_pipeline = 0;
  int day = 0;

  for (int i = 1; i < argc; i++) {
    if (*argv[i] == '-') {
      if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
        debug_pipeline = 1;
      } else {
        goto usage;
        return 1;
      }
    } else {
      day = atoi(argv[i]);
    }
  }

  if (day < 1 || day > 25) {
  usage:
    fprintf(stderr, "Usage: %s [--debug] <day number>\n", argv[0]);
    return 64;
  }

  char filename[128];
  if (!snprintf(filename, sizeof(filename), "src/day%d.pipe", day)) {
    __builtin_trap();
    return 1;
  }

  FILE *fp = fopen(filename, "r");
  if (fp == NULL) {
    fprintf(stderr, "failed to open %s: %s", filename, strerror(errno));
    return 1;
  }

  if (fseek(fp, 0, SEEK_END)) {
    fprintf(stderr, "failed to seek to end of %s: %s", filename,
            strerror(errno));
    return 1;
  }
  fpos_t pos;
  if (fgetpos(fp, &pos)) {
    fprintf(stderr, "failed to get size of %s: %s", filename, strerror(errno));
    return 1;
  }
  off_t size = pos.__pos;
  char *program = malloc(size + 1);
  fseek(fp, 0, SEEK_SET);
  if (fread(program, 1, size, fp) != size) {
    fprintf(stderr, "failed to read %s: %s", filename, strerror(errno));
    return 1;
  }
  program[size] = '\0';
  fclose(fp);

  pipeline_t *pipeline = parse_pipeline(program);
  free(program);
  if (pipeline == NULL) {
    fprintf(stderr, "failed to parse pipeline\n");
    return 1;
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

  pipeline->input = input_buffer;

  if (debug_pipeline)
    debug_print_pipeline(pipeline->head, 0);

  pipeline_process(pipeline);

  pipeline_free(pipeline);

  return 0;
}
