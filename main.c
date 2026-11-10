#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#define TODO_DEFAULT_DATA_PATH "./todos.md"
#define TODO_ID_MAX_DIGITS 20 /* UINT64_MAX has 20 decimal digits */

#define FNV64_OFFSET_BASIS 14695981039346656037ULL
#define FNV64_PRIME 1099511628211ULL

static unsigned long long Fnv1a64Update(unsigned long long h, const void *data, size_t size) {
  const unsigned char *p = data;
  for (size_t i = 0; i < size; i++) {
    h ^= p[i];
    h *= FNV64_PRIME;
  }
  return h;
}

static const char *DataPath(void) {
  const char *env = getenv("TODO_FILE");
  return (env != NULL && env[0] != '\0') ? env : TODO_DEFAULT_DATA_PATH;
}

static void PrintUsage(FILE *out) {
  fprintf(out,
    "Usage:\n"
    "  list                       List current todos\n"
    "  add <title> <description>  Add a todo\n"
    "  rm <id> [id...]            Remove todos by id\n"
    "\n"
    "Data file: $TODO_FILE, or %s if unset\n",
    TODO_DEFAULT_DATA_PATH);
}

/* Extracts the id (first space-delimited token) from a line.
 * Returns 1 on success, 0 if the line has no id. */
static int ParseId(const char *line, char id[TODO_ID_MAX_DIGITS + 1]) {
  return sscanf(line, "%20[^ \n]", id) == 1;
}

typedef struct {
  char (*items)[TODO_ID_MAX_DIGITS + 1];
  size_t count;
  size_t cap;
} IdSet;

static int CompareIds(const void *a, const void *b) {
  return strcmp(a, b);
}

/* Reads every id in the file once, sorted for binary search.
 * Returns 0 on success, -1 on error (errno set). */
static int LoadIds(FILE *f, IdSet *set) {
  char *line = NULL;
  size_t lineCap = 0;
  int result = 0;

  while (getline(&line, &lineCap, f) != -1) {
    char id[TODO_ID_MAX_DIGITS + 1];
    if (!ParseId(line, id))
      continue;
    if (set->count == set->cap) {
      size_t newCap = set->cap ? set->cap * 2 : 64;
      void *grown = realloc(set->items, newCap * sizeof *set->items);
      if (grown == NULL) {
        result = -1;
        break;
      }
      set->items = grown;
      set->cap = newCap;
    }
    memcpy(set->items[set->count++], id, sizeof id);
  }
  if (result == 0 && ferror(f))
    result = -1;

  int savedErrno = errno;
  free(line);
  errno = savedErrno; /* keep the error for the caller's perror() */

  if (result == 0 && set->count > 0)
    qsort(set->items, set->count, sizeof *set->items, CompareIds);
  return result;
}

static int IdSetContains(const IdSet *set, const char *id) {
  return set->count > 0 &&
         bsearch(id, set->items, set->count, sizeof *set->items, CompareIds) != NULL;
}

static int CmdList(const char *path) {
  FILE *todosFile = fopen(path, "r");
  if (todosFile == NULL) {
    /* Only a missing file means "no todos yet"; anything else is a real error. */
    if (errno == ENOENT) {
      printf("No todos yet.\n");
      return EXIT_SUCCESS;
    }
    perror("Error: could not open data file");
    return EXIT_FAILURE;
  }

  int ch;
  int any = 0;
  while ((ch = fgetc(todosFile)) != EOF) {
    putchar(ch);
    any = 1;
  }

  int failed = ferror(todosFile);
  fclose(todosFile);
  if (failed) {
    fprintf(stderr, "Error: failed reading %s\n", path);
    return EXIT_FAILURE;
  }
  if (!any)
    printf("No todos yet.\n");
  return EXIT_SUCCESS;
}

static int CmdAdd(const char *path, const char *title, const char *description) {
  size_t data_size = strlen(title) + strlen(description) + 2; /* space + NUL */
  char *data = malloc(data_size);
  if (data == NULL) {
    fprintf(stderr, "Error: out of memory\n");
    return EXIT_FAILURE;
  }
  snprintf(data, data_size, "%s %s", title, description);

  /* Newlines would break the one-task-per-line file format. */
  for (char *p = data; *p != '\0'; ++p)
    if (*p == '\n' || *p == '\r')
      *p = ' ';

  /* "a+": reads start at the beginning, writes always append, file is
   * created if missing. One handle serves both the id scan and the write. */
  FILE *todosFile = fopen(path, "a+");
  if (todosFile == NULL) {
    perror("Error: could not open data file");
    free(data);
    return EXIT_FAILURE;
  }

  IdSet ids = {0};
  if (LoadIds(todosFile, &ids) != 0) {
    perror("Error: could not read data file");
    free(ids.items);
    fclose(todosFile);
    free(data);
    return EXIT_FAILURE;
  }

  /* Hash the text; on collision with an existing id, mix in a counter. */
  unsigned long long hash = Fnv1a64Update(FNV64_OFFSET_BASIS, data, strlen(data));
  char id[TODO_ID_MAX_DIGITS + 1];
  for (unsigned long long salt = 1;; ++salt) {
    snprintf(id, sizeof id, "%llu", hash);
    if (!IdSetContains(&ids, id))
      break;
    hash = Fnv1a64Update(hash, &salt, sizeof salt);
  }
  free(ids.items);

  /* Switching from reading to writing on an update stream needs a seek. */
  fseek(todosFile, 0, SEEK_END);
  int writeFailed = fprintf(todosFile, "%s - %s\n", id, data) < 0;
  writeFailed |= fclose(todosFile) != 0;
  free(data);

  if (writeFailed) {
    perror("Error: could not write task");
    return EXIT_FAILURE;
  }

  printf("Task added with id %s\n", id);
  return EXIT_SUCCESS;
}

static int CmdRemove(const char *path, int idCount, char *ids[]) {
  FILE *todosFile = fopen(path, "r");
  if (todosFile == NULL) {
    perror("Error: could not open data file");
    return EXIT_FAILURE;
  }

  size_t tmpPathSize = strlen(path) + sizeof ".tmp";
  char *tmpPath = malloc(tmpPathSize);
  if (tmpPath == NULL) {
    fprintf(stderr, "Error: out of memory\n");
    fclose(todosFile);
    return EXIT_FAILURE;
  }
  snprintf(tmpPath, tmpPathSize, "%s.tmp", path);

  FILE *tmpFile = fopen(tmpPath, "w");
  if (tmpFile == NULL) {
    perror("Error: could not open temp file");
    fclose(todosFile);
    free(tmpPath);
    return EXIT_FAILURE;
  }

  char *line = NULL;
  size_t cap = 0;
  ssize_t read;
  int removed = 0;
  int ioFailed = 0;
  while ((read = getline(&line, &cap, todosFile)) != -1) {
    char id[TODO_ID_MAX_DIGITS + 1];
    int keep = 1;
    if (ParseId(line, id)) {
      for (int i = 0; i < idCount; ++i) {
        if (strcmp(id, ids[i]) == 0) {
          keep = 0;
          break;
        }
      }
    }

    if (keep) {
      if (fputs(line, tmpFile) == EOF)
        ioFailed = 1;
    } else {
      ++removed;
    }
  }
  if (ferror(todosFile))
    ioFailed = 1;

  free(line);
  fclose(todosFile);
  if (fclose(tmpFile) != 0)
    ioFailed = 1;

  if (ioFailed) {
    fprintf(stderr, "Error: I/O failure, data file left unchanged\n");
    remove(tmpPath);
    free(tmpPath);
    return EXIT_FAILURE;
  }

  if (removed == 0) {
    fprintf(stderr, "No matching task found; nothing removed\n");
    remove(tmpPath);
    free(tmpPath);
    return EXIT_FAILURE;
  }

  /* rename() atomically replaces the destination on POSIX. */
  if (rename(tmpPath, path) != 0) {
    perror("Error: could not replace data file");
    remove(tmpPath);
    free(tmpPath);
    return EXIT_FAILURE;
  }

  free(tmpPath);
  printf("Removed %d task%s\n", removed, removed == 1 ? "" : "s");
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "No command provided\n\n");
    PrintUsage(stderr);
    return EXIT_FAILURE;
  }

  const char *path = DataPath();
  const char *cmd = argv[1];

  if (strcmp(cmd, "list") == 0) {
    return CmdList(path);
  }

  if (strcmp(cmd, "add") == 0) {
    if (argc != 4) {
      fprintf(stderr, "add needs exactly a title and a description (quote multi-word values)\n\n");
      PrintUsage(stderr);
      return EXIT_FAILURE;
    }
    return CmdAdd(path, argv[2], argv[3]);
  }

  if (strcmp(cmd, "rm") == 0) {
    if (argc < 3) {
      fprintf(stderr, "rm needs at least one id\n\n");
      PrintUsage(stderr);
      return EXIT_FAILURE;
    }
    return CmdRemove(path, argc - 2, argv + 2);
  }

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
    PrintUsage(stdout);
    return EXIT_SUCCESS;
  }

  fprintf(stderr, "Invalid operation: %s\n\n", cmd);
  PrintUsage(stderr);
  return EXIT_FAILURE;
}
