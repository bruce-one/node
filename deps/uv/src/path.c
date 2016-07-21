#include "uv-common.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <limits.h> /* PATH_MAX */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Resolves . and .. elements in a path with directory names */
static
ssize_t normalize_string_win32(const uv_buf_t* path, uv_buf_t* res, int allow_above_root) {
  ssize_t last_slash;
  int dots;
  char code;
  ssize_t i;
  
  last_slash = -1;
  dots = 0;
  code = 0;


  /* path->len is an upper bound of the return value's length;
   * use it for allocating it. */
  res->len = 0;
  res->base = uv__malloc(path->len < 4 ? 5 : path->len + 1);
  if (res->base == NULL) return -ENOMEM;

  for (i = 0; (size_t)i <= path->len; ++i) {
    if ((size_t)i < path->len)
      code = path->base[i];
    else if (code == '/' || code == '\\')
      break;
    else
      code = '/';

    if (code == '/' || code == '\\') {
      if (last_slash == i - 1 || dots == 1) {
        /* NOOP */
      } else if (last_slash != i - 1 && dots == 2) {
        if (res->len < 2 ||
            res->base[res->len - 1] != '.' ||
            res->base[res->len - 2] != '.') {
          if (res->len > 2) {
            ssize_t start, j;
            start = res->len - 1;
            j = start;

            for (; j >= 0; --j) {
              if (res->base[j] == '\\')
                break;
            }
            if (j != start) {
              if (j == -1) {
                res->len = 0;
              } else {
                res->len = j;
              }
              last_slash = i;
              dots = 0;
              continue;
            }
          } else if (res->len == 2 || res->len == 1) {
            res->len = 0;
            last_slash = i;
            dots = 0;
            continue;
          }
        }
        if (allow_above_root) {
          if (res->len > 0) {
            /* res += '\\..'; */
            res->base[res->len++] = '\\';
            res->base[res->len++] = '.';
            res->base[res->len++] = '.';
          } else {
            /* res = '..'; */
            res->len = 2;
            res->base[0] = res->base[1] = '.';
          }
        }
      } else {
        if (res->len > 0) {
          /* res += '\\' + path.slice(last_slash + 1, i); */

          res->base[res->len++] = '\\';
        } else {
          /* res = path.slice(last_slash + 1, i); */
        }

        memcpy(res->base + res->len, path->base + last_slash + 1, i - (last_slash + 1));
        res->len += i - (last_slash + 1);
      }
      last_slash = i;
      dots = 0;
    } else if (code == '.' && dots != -1) {
      ++dots;
    } else {
      dots = -1;
    }
  }

  res->base[res->len] = '\0';
  return res->len;
}

/* Resolves . and .. elements in a path with directory names */
static
ssize_t normalize_string_posix(const uv_buf_t* path, uv_buf_t* res, int allow_above_root, size_t head_space) {
  ssize_t last_slash;
  ssize_t dots;
  char code;
  ssize_t i;

  last_slash = -1;
  dots = 0;
  code = 0;

  /* path->len is an upper bound of the return value's length;
   * use it for allocating it. */
  res->len = 0;
  res->base = uv__malloc(path->len < 4 ? 5 : path->len + 1 + head_space);
  if (res->base == NULL) return -ENOMEM;
  res->base += head_space;

  for (i = 0; (size_t)i <= path->len; ++i) {
    if ((size_t)i < path->len)
      code = path->base[i];
    else if (code == '/')
      break;
    else
      code = '/';
    if (code == '/') {
      if (last_slash == i - 1 || dots == 1) {
        /* NOOP */
      } else if (last_slash != i - 1 && dots == 2) {
        if (res->len < 2 ||
            res->base[res->len - 1] != '.' ||
            res->base[res->len - 2] != '.') {
          if (res->len > 2) {
            ssize_t start, j;
            start = res->len - 1;
            j = start;

            for (; j >= 0; --j) {
              if (res->base[j] == '/')
                break;
            }
            if (j != start) {
              if (j == -1)
                res->len = 0;
              else
                res->len = j;
              last_slash = i;
              dots = 0;
              continue;
            }
          } else if (res->len == 2 || res->len == 1) {
            res->len = 0;
            last_slash = i;
            dots = 0;
            continue;
          }
        }
        if (allow_above_root) {
          if (res->len > 0) {
            /* res += '/..'; */
            res->base[res->len++] = '/';
            res->base[res->len++] = '.';
            res->base[res->len++] = '.';
          } else {
            /* res = '..'; */
            res->len = 2;
            res->base[0] = res->base[1] = '.';
          }
        }
      } else {
        if (res->len > 0) {
          /* res += '/' + path.slice(last_slash + 1, i); */

          res->base[res->len++] = '/';
        } else {
          /* res = path.slice(last_slash + 1, i); */
        }

        memcpy(res->base + res->len, path->base + last_slash + 1, i - (last_slash + 1));
        res->len += i - (last_slash + 1);
      }
      last_slash = i;
      dots = 0;
    } else if (code == '.' && dots != -1) {
      ++dots;
    } else {
      dots = -1;
    }
  }

  res->base[res->len] = '\0';
  res->base -= head_space;
  return res->len;
}

UV_EXTERN
ssize_t path_win32_resolve(const uv_buf_t* from, const uv_buf_t* to, uv_buf_t* res, size_t* resolved_device_length_) {
  uv_buf_t resolved_device;
  uv_buf_t resolved_tail;
  uv_buf_t device;
  uv_buf_t new_resolved_tail;

  int resolved_absolute;
  int err;

  ssize_t i;

  resolved_absolute = 0;
  err = 0;
  device.len = 0;
  device.base = NULL;
  resolved_device.len = 0;
  resolved_device.base = NULL;
  resolved_tail.len = 0;
  resolved_tail.base = NULL;
  new_resolved_tail.len = 0;
  new_resolved_tail.base = NULL;

  for (i = 1; i >= -1; i--) {
#ifdef _WIN32
    /* MAX_PATH is in characters, not bytes. Make sure we have enough headroom. */
    char cwd_buf[MAX_PATH * 4];
#else
    char cwd_buf[PATH_MAX];
#endif

    const char* path;
    size_t len;
    int is_absolute;

    size_t root_end;
    char code;

    len = 0;

    if (i == 1) {
      path = to ? to->base : NULL;
      len = to ? to->len : 0;
    } else if (i == 0) {
      path = from ? from->base : NULL;
      len = from ? from->len : 0;
    } else if (resolved_device.len == 0) {
      /* path = process.cwd(); */
      len = sizeof(cwd_buf);
      err = uv_cwd(cwd_buf, (size_t*)&len);
      if (err)
        return -err;
      path = cwd_buf;
    } else {
      /* Windows has the concept of drive-specific current working
       * directories. If we've resolved a drive letter but not yet an
       * absolute path, get cwd for that drive. We're sure the device is not
       * a UNC path at this points, because UNC paths are always absolute. */

      /* path = process.env['=' + resolvedDevice]; */
      cwd_buf[0] = '=';
      if (resolved_device.len + 1 >= sizeof(cwd_buf)) { err = -ENAMETOOLONG; goto fail; }
      memcpy(cwd_buf + 1, resolved_device.base, resolved_device.len);
      cwd_buf[resolved_device.len + 1] = '\0';
      path = getenv(cwd_buf);
      len = strlen(path);

      /* Verify that a drive-local cwd was found and that it actually points
       * to our drive. If not, default to the drive's root. */
      if (path == NULL ||
          resolved_device.len != 2 ||
          tolower(path[0]) != tolower(resolved_device.base[0]) ||
          path[1] != resolved_device.base[1] ||
          path[2] != '\\') {
        /* path = resolvedDevice + '\\'; */
        len = resolved_device.len;
        if (len > sizeof(cwd_buf)) { err = -ENAMETOOLONG; goto fail; }
        memcpy(cwd_buf, resolved_device.base, len);
        cwd_buf[len++] = '\\';
        path = cwd_buf;
      }
    }

    /* Skip empty entries */
    if (len == 0)
      continue;

    root_end = 0;
    code = path[0];

    assert(device.base == NULL);
    device.len = 0;
    is_absolute = 0;

    /* Try to match a root */
    if (len > 1) {
      if (code == '/' || code == '\\') {
        /* Possible UNC root */

        /* If we started with a separator, we know we at least have an
         * absolute path of some kind (UNC or otherwise) */
        is_absolute = 1;

        code = path[1];
        if (code == '/' || code == '\\') {
          /* Matched double path separator at beginning */
          size_t j, last;
          j = 2;
          last = j;
          /* Match 1 or more non-path separators */
          for (; j < len; ++j) {
            code = path[j];
            if (code == '/' || code == '\\')
              break;
          }
          if (j < len && j != last) {
            /* const firstPart = path.slice(last, j); */
            const char* first_part;
            size_t first_part_length;
            first_part = path + last;
            first_part_length = j - last;
            /* Matched! */
            last = j;
            /* Match 1 or more path separators */
            for (; j < len; ++j) {
              code = path[j];
              if (code != '/' && code != '\\')
                break;
            }
            if (j < len && j != last) {
              /* Matched! */
              last = j;
              /* Match 1 or more non-path separators */
              for (; j < len; ++j) {
                code = path[j];
                if (code == '/' || code == '\\')
                  break;
              }
              if (j == len) {
                /* We matched a UNC root only */

                /* device = '\\\\' + firstPart + '\\' + path.slice(last); */
                device.base = uv__malloc(3 + first_part_length + (len - last));
                if (device.base == NULL) { err = -ENOMEM; goto fail; }
                device.base[0] = '\\';
                device.base[1] = '\\';
                memcpy(device.base + 2, first_part, first_part_length);
                device.base[2 + first_part_length] = '\\';
                device.len = 2 + first_part_length + 1;
                memcpy(device.base + device.len, path + last, len - last);
                device.len += len - last;

                root_end = j;
              } else if (j != last) {
                /* We matched a UNC root with leftovers */

                /* device = '\\\\' + firstPart + '\\' + path.slice(last, j); */
                device.base = uv__malloc(4 + first_part_length + (j - last));
                if (device.base == NULL) { err = -ENOMEM; goto fail; }
                device.base[0] = '\\';
                device.base[1] = '\\';
                memcpy(device.base + 2, first_part, first_part_length);
                device.base[2 + first_part_length] = '\\';
                device.len = 2 + first_part_length + 1;
                memcpy(device.base + device.len, path + last, j - last);
                device.len += j - last;

                root_end = j;
              }
            }
          }
        } else {
          root_end = 1;
        }
      } else if ((code >= 'A' && code <= 'Z') ||
                 (code >= 'a' && code <= 'z')) {
        /* Possible device root */

        code = path[1];
        if (path[1] == ':') {
          device.base = uv__malloc(2);
          if (device.base == NULL) { err = -ENOMEM; goto fail; }
          memcpy(device.base, path, 2);
          device.len = 2;
          root_end = 2;
          if (len > 2) {
            code = path[2];
            if (code == '/' || code == '\\') {
              /* Treat separator following drive name as an absolute path
               * indicator */
              is_absolute = 1;
              root_end = 3;
            }
          }
        }
      }
    } else if (code == '/' || code == '\\') {
      /* `path` contains just a path separator */
      root_end = 1;
      is_absolute = 1;
    }

    if (device.len > 0 &&
        resolved_device.len > 0 &&
        (device.len != resolved_device.len ||
         strncasecmp(device.base, resolved_device.base, device.len) != 0)) {
      /* This path points to another device so it is not applicable */
      uv__free(device.base);
      device.base = NULL;
      continue;
    }

    if (resolved_device.len == 0 && device.len > 0) {
      assert(resolved_device.base == NULL);
      resolved_device.len = device.len;
      resolved_device.base = device.base;
      device.base = NULL;
    }
    if (!resolved_absolute) {
      /* resolvedTail = path.slice(rootEnd) + '\\' + resolvedTail; */
      char* new_tail;
      new_tail = uv__realloc(resolved_tail.base, resolved_tail.len + (len - root_end) + 1);
      if (new_tail == NULL) { err = -ENOMEM; goto fail; }
      resolved_tail.base = new_tail;

      memmove(resolved_tail.base + (len - root_end + 1),
              resolved_tail.base,
              resolved_tail.len);
      resolved_tail.len += len - root_end + 1;
      memcpy(resolved_tail.base, path + root_end, len - root_end);
      resolved_tail.base[len - root_end] = '\\';

      resolved_absolute = is_absolute;
    }

    uv__free(device.base);
    device.base = NULL;

    if (resolved_device.len > 0 && resolved_absolute) {
      break;
    }
  }

  /* At this point the path should be resolved to a full absolute path,
   * but handle relative paths to be safe (might happen when process.cwd()
   * fails) */

  /* Normalize the tail path */
  err = normalize_string_win32(&resolved_tail, &new_resolved_tail, !resolved_absolute);
  if (err < 0) goto fail;

  if (resolved_device_length_)
    *resolved_device_length_ = resolved_device.len;

  if (resolved_device.len == 0 &&
      !resolved_absolute &&
      resolved_tail.len == 0) {
    free(resolved_device.base);
    free(resolved_tail.base);
    free(new_resolved_tail.base);
    res->base = uv__strdup(".");
    return res->len = 1;
  }

  res->base = uv__realloc(resolved_device.base, resolved_device.len + 2 + new_resolved_tail.len);
  if (!res->base) { err = -ENOMEM; goto fail; }
  resolved_device.base = NULL;

  if (resolved_absolute)
    res->base[resolved_device.len] = '\\';
  res->len = resolved_device.len + 1;
  memcpy(res->base + res->len, new_resolved_tail.base, new_resolved_tail.len);
  res->len += new_resolved_tail.len;
  res->base[res->len] = '\0';
  err = res->len;

fail:
  assert(device.base == NULL);
  free(resolved_device.base);
  free(resolved_tail.base);
  free(new_resolved_tail.base);
  return err;
}

UV_EXTERN
ssize_t path_posix_resolve(const uv_buf_t* from, const uv_buf_t* to, uv_buf_t* res, size_t* resolved_device_length_) {
  uv_buf_t resolved_path;
  int resolved_absolute;
  ssize_t i;
  ssize_t err;

  resolved_path.len = 0;
  resolved_path.base = NULL;

  resolved_absolute = 0;
  err = 0;

  for (i = 1; i >= -1 && !resolved_absolute; i--) {
    uv_buf_t path;
    char* new_resolved_path;
#ifdef _WIN32
    /* MAX_PATH is in characters, not bytes. Make sure we have enough headroom. */
    char cwd_buf[MAX_PATH * 4];
#else
    char cwd_buf[PATH_MAX];
#endif

    if (i == 1) {
      path.base = to ? to->base : NULL;
      path.len = to ? to->len : 0;
    } else if (i == 0) {
      path.base = from ? from->base : NULL;
      path.len = from ? from->len : 0;
    } else {
      path.len = PATH_MAX;
      path.base = cwd_buf;
      err = uv_cwd(cwd_buf, (size_t*)&path.len);
      if (err) { goto fail; }
    }

    /* Skip empty entries */
    if (path.len == 0) continue;

    /* resolvedPath = path + '/' + resolvedPath; */
    new_resolved_path = realloc(resolved_path.base, resolved_path.len + path.len + 1);
    if (new_resolved_path == NULL) { err = -ENOMEM; goto fail; }
    resolved_path.base = new_resolved_path;
    memmove(resolved_path.base + path.len + 1, resolved_path.base, resolved_path.len);
    resolved_path.base[path.len] = '/';
    memcpy(resolved_path.base, path.base, path.len);
    resolved_path.len += path.len + 1;

    resolved_absolute = path.base[0] == '/';
  }

  /* At this point the path should be resolved to a full absolute path, but
   * handle relative paths to be safe (might happen when process.cwd() fails) */

  /* Normalize the path */
  err = normalize_string_posix(&resolved_path, res, !resolved_absolute, resolved_absolute);
  if (err < 0) goto fail;

  free(resolved_path.base);

  if (resolved_device_length_)
    *resolved_device_length_ = resolved_absolute;

  if (resolved_absolute) {
    res->base[0] = '/';
    res->len++;
    return res->len;
  } else if (res->len > 0) {
    return res->len;
  } else {
    free(res->base);
    res->base = uv__strdup(".");
    return res->len = 1;
  }

fail:
  free(resolved_path.base);
  return err;
}
