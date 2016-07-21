#include "uv-common.h"
#include <string.h>

ssize_t path_win32_resolve(const uv_buf_t* from, const uv_buf_t* to, uv_buf_t* res, size_t* resolved_device_length_);
ssize_t path_posix_resolve(const uv_buf_t* from, const uv_buf_t* to, uv_buf_t* res, size_t* resolved_device_length_);

#ifdef _WIN32
#define path_resolve path_win32_resolve
#else
#define path_resolve path_posix_resolve
#endif


#define intcmp(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)

#ifndef _WIN32
struct symlink_map_entry_s {
  uint64_t dev;
  uint64_t ino;
  uv_buf_t target;

  RB_ENTRY(symlink_map_entry_s) tree_entry;
};

static inline
int symlink_map_compare(const struct symlink_map_entry_s* a, const struct symlink_map_entry_s* b) {
  if (a->dev != b->dev)
    return intcmp(a->dev, b->dev);
  return intcmp(a->ino, b->ino);
}

RB_HEAD(symlink_map_s, symlink_map_entry_s);
RB_GENERATE_STATIC(symlink_map_s, symlink_map_entry_s, tree_entry, symlink_map_compare)

static void symlink_map_find(struct symlink_map_s* seen_links, const uv_stat_t* statbuf, uv_buf_t* link_target) {
  struct symlink_map_entry_s search_dummy, * entry;

  search_dummy.dev = statbuf->st_dev;
  search_dummy.ino = statbuf->st_ino;
  entry = RB_FIND(symlink_map_s, seen_links, &search_dummy);
  if (entry != NULL) {
    link_target->base = entry->target.base;
    link_target->len = entry->target.len;
  } else {
    link_target->base = NULL;
  }
}

static int symlink_map_insert_steal(struct symlink_map_s* seen_links, const uv_stat_t* statbuf, const uv_buf_t* link_target) {
  struct symlink_map_entry_s* new_node, * existing;
  new_node = uv__malloc(sizeof(struct symlink_map_entry_s));
  if (new_node == NULL)
    return -ENOMEM;
  new_node->dev = statbuf->st_dev;
  new_node->ino = statbuf->st_ino;
  new_node->target.base = link_target->base;
  new_node->target.len = link_target->len;
  existing = RB_INSERT(symlink_map_s, seen_links, new_node);
  assert(existing == NULL);
  return 0;
}

#endif

struct string_set_entry_s {
  uv_buf_t key;

  RB_ENTRY(string_set_entry_s) tree_entry;
};

static inline
int string_set_compare(const struct string_set_entry_s* a, const struct string_set_entry_s* b) {
  if (a->key.len != b->key.len)
    return intcmp(a->key.len, b->key.len);
  return memcmp(a->key.base, b->key.base, a->key.len);
}

RB_HEAD(string_set_s, string_set_entry_s);
RB_GENERATE_STATIC(string_set_s, string_set_entry_s, tree_entry, string_set_compare)

static int string_set_has(struct string_set_s* set, const uv_buf_t* str) {
  /* Meh. Would like something better here than weird casting. */
  return RB_FIND(string_set_s, set, (struct string_set_entry_s*) str) != NULL;
}

static int string_set_insert_steal(struct string_set_s* set, const uv_buf_t* str) {
  struct string_set_entry_s* new_node, * existing;
  new_node = uv__malloc(sizeof(struct string_set_entry_s));
  if (new_node == NULL)
    return -ENOMEM;
  new_node->key.base = str->base;
  new_node->key.len = str->len;
  existing = RB_INSERT(string_set_s, set, new_node);
  assert(existing == NULL);
  return 0;
}

static void free_symlink_map(struct symlink_map_entry_s* head) {
  if (!head) return;
  free_symlink_map(head->tree_entry.rbe_left);
  free_symlink_map(head->tree_entry.rbe_right);
  uv__free(head->target.base);
  uv__free(head);
}

static void free_string_set(struct string_set_entry_s* head) {
  if (!head) return;
  free_string_set(head->tree_entry.rbe_left);
  free_string_set(head->tree_entry.rbe_right);
  uv__free(head->key.base);
  uv__free(head);
}

/* Currently only synchronous. */
UV_EXTERN
ssize_t uv_fs_realpath_x(uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb) {
  uv_buf_t p;
  uv_buf_t path_;

  ssize_t err;

#ifndef _WIN32
  struct symlink_map_s seen_links = RB_INITIALIZER();
#endif
  struct string_set_s known_hard = RB_INITIALIZER();

  size_t new_pos;
  char* new_current;

  /* current character position in p */
  size_t pos;
  /* the partial path so far, including a trailing slash if any */
  uv_buf_t current;
  /* the partial path without a trailing slash (except when pointing at a root) */
  uv_buf_t base;
  /* pointer to release on exit if base is not currently owned by the known_hard set */
  char* base_release;
  /* the partial path scanned in the previous round, with slash */
  uv_buf_t previous;

  uv_buf_t link_target;

  uv_fs_t req_;
  uv_stat_t statbuf;

  assert(cb == NULL);

  base_release = NULL;
  pos = 0;
  previous.base = base.base = current.base = p.base = link_target.base = NULL;
  previous.len = base.len = current.len = p.len = link_target.len = 0;

  memset(req, 0, sizeof(*req));

  req->type = UV_FS;
  req->fs_type = UV_FS_REALPATH;
  req->loop = loop;
  req->path = path;
  req->new_path = NULL;
  req->cb = cb;

  /* make p is absolute */
  path_.len = strlen(path);
  path_.base = (char*)path;
  err = path_resolve(&path_, NULL, &p, NULL);
  if (err < 0) goto fail;

#define start()                                                               \
  do {                                                                        \
    /* Skip over roots */                                                     \
    /* In JS, this used a regex. Since the path_resolve implementation */     \
    /* already provides root detection, use that. */                          \
    uv__free(current.base);                                                   \
    current.base = NULL;                                                      \
    err = path_resolve(&p, NULL, &current, &pos);                             \
    if (err < 0) goto fail;                                                   \
    current.len = pos;                                                        \
    uv__free(base_release);                                                   \
    base_release = base.base = uv__strndup(current.base, pos);                \
    if (!current.base || !base.base) { err = -ENOMEM; goto fail; }            \
    base.len = current.len = pos;                                             \
    previous.len = 0;                                                         \
                                                                              \
    start_check_root_on_windows();                                            \
  } while(0)

/* Commented out for testing only; it doesn't hurt to do this on UNIX. */
/* #ifdef _WIN32 */
#define start_check_root_on_windows()                                         \
  do {                                                                        \
    /* On windows, check that the root exists. On unix there is no need. */   \
    if (!string_set_has(&known_hard, &base)) {                                \
      err = uv_fs_lstat(loop, &req_, base.base, NULL);                        \
      uv_fs_req_cleanup(&req_);                                               \
      if (err < 0) goto fail;                                                 \
      err = string_set_insert_steal(&known_hard, &base);                      \
      if (err < 0) goto fail;                                                 \
      base_release = NULL;                                                    \
    }                                                                         \
  } while(0)
/*#else
  #define start_check_root_on_windows()
  #endif*/

  start();

  /* walk down the path, swapping out linked pathparts for their real
   * values
   * NB: p.len changes. */
  while (pos < p.len) {
    /* find the next part */
    char* end_of_next_part;
    int found_slash;
    end_of_next_part = memchr(p.base + pos, '/', p.len - pos);
#ifdef _WIN32
    {
      char* backslash_end_of_next_part;
      backslash_end_of_next_part = memchr(p.base + pos, '\\', p.len - pos);
      if (end_of_next_part == NULL) {
        end_of_next_part = backslash_end_of_next_part;
      } else if (backslash_end_of_next_part != NULL && backslash_end_of_next_part < end_of_next_part) {
        end_of_next_part = backslash_end_of_next_part;
      }
    }
#endif
    found_slash = end_of_next_part != NULL ? 1 : 0;
    if (!found_slash)
      end_of_next_part = p.base + p.len;
    new_pos = end_of_next_part - p.base + 1;

    /* previous = current;
     * current += result[0]; */
    new_current = uv__realloc(previous.base, current.len + (new_pos - pos) + 1);
    if (new_current == NULL) { err = -ENOMEM; goto fail; }
    previous.base = current.base;
    previous.len = current.len;
    memcpy(new_current, current.base, current.len);
    memcpy(new_current + current.len, p.base + pos, new_pos - pos);
    current.base = new_current;
    current.len += new_pos - pos;
    current.base[current.len] = '\0';

    /* base = previous + result[1]; */
    base.len = current.len - found_slash;
    uv__free(base_release);
    base_release = base.base = uv__malloc(base.len + 1);
    if (!base.base) { err = -ENOMEM; goto fail; }
    memcpy(base.base, current.base, base.len);
    base.base[base.len] = '\0';

    /* pos = nextPartRe.lastIndex; */
    pos = new_pos;
    if (pos > p.len) pos = p.len;

    /* continue if not a symlink */
    if (string_set_has(&known_hard, &base))
      continue;

    err = uv_fs_lstat(loop, &req_, base.base, NULL);
    memcpy(&statbuf, &req_.statbuf, sizeof(statbuf));
    uv_fs_req_cleanup(&req_);
    if (err < 0) goto fail;
    if ((statbuf.st_mode & S_IFMT) != S_IFLNK) {
      err = string_set_insert_steal(&known_hard, &base);
      if (err < 0) goto fail;
      base_release = NULL;
      continue;
    }

    /* read the link if it wasn't read before
    * dev/ino always return 0 on windows, so skip the check. */
    link_target.base = NULL;

#ifndef _WIN32
    symlink_map_find(&seen_links, &statbuf, &link_target);
#endif
    if (link_target.base == NULL) {
      /* Doing a simple, otherwise pointless access() to catch ELOOPs. */
      /* This originally was a stat() call, but I remember there was discussion
       * about using access() instead and it made sense to me. */
      err = uv_fs_access(loop, &req_, base.base, F_OK, NULL);
      uv_fs_req_cleanup(&req_);
      if (err < 0) goto fail;

      err = uv_fs_readlink(loop, &req_, base.base, NULL);
      if (err < 0) {
        uv_fs_req_cleanup(&req_);
        goto fail;
      }
      link_target.base = req_.ptr;
      req_.ptr = NULL;
      uv_fs_req_cleanup(&req_);
      link_target.len = strlen(link_target.base);
#ifndef _WIN32
      err = symlink_map_insert_steal(&seen_links, &statbuf, &link_target);
      if (err < 0) {
        uv__free(link_target.base);
        goto fail;
      }
#endif
    }

    {
      uv_buf_t resolved_link;
      uv_buf_t p_slice;
      char* old_p;
      err = path_resolve(&previous, &link_target, &resolved_link, NULL);
      if (err < 0) goto fail;

      /* resolve the link, then start over
       * p = pathModule.resolve(resolvedLink, p.slice(pos)); */
      p_slice.base = p.base + pos;
      p_slice.len = p.len - pos;
      old_p = p.base;
      err = path_resolve(&resolved_link, &p_slice, &p, NULL);
      uv__free(resolved_link.base);
      uv__free(old_p);
      if (err < 0) goto fail;
    }

    start();
  }

  err = 0;

  p.base[p.len] = '\0';
  req->ptr = p.base;
fail:
  uv__free(current.base);
  uv__free(base_release);
  uv__free(previous.base);

#ifndef _WIN32
  free_symlink_map(seen_links.rbh_root);
#endif
  free_string_set(known_hard.rbh_root);

  if (err < 0)
    uv__free(p.base);

  return req->result = err;
}
