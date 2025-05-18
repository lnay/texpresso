/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef SPROTOCOL_H
#define SPROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "myabort.h"
#include "pic_cache.h"

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <variant>
#include <optional>
#endif

typedef int file_id;

#define LOG 0
#define BUF_SIZE 4096

#define LEN(txt) (sizeof(txt)-1)
#define STR(X) #X
#define SSTR(X) STR(X)

#define pabort() \
  do { perror(__FILE__ ":" SSTR(__LINE__)); myabort(); } while(0)

#define mabort(...) \
  do { fprintf(stderr, "Aborting from " __FILE__ ":" SSTR(__LINE__) "\n" __VA_ARGS__); print_backtrace(); abort(); } while(0)

#define PACK(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)

/* QUERIES */

namespace query {
    enum message : uint32_t {
      Q_OPEN = PACK('O','P','E','N'),
      Q_READ = PACK('R','E','A','D'),
      Q_WRIT = PACK('W','R','I','T'),
      Q_CLOS = PACK('C','L','O','S'),
      Q_SIZE = PACK('S','I','Z','E'),
      Q_SEEN = PACK('S','E','E','N'),
      Q_GPIC = PACK('G','P','I','C'),
      Q_SPIC = PACK('S','P','I','C'),
      Q_CHLD = PACK('C','H','L','D'),
    };

    struct open {
      file_id fid;
      char *path, *mode;
    };
    struct read {
      file_id fid;
      int pos, size;
    };
    struct writ {
      file_id fid;
      int pos, size;
      char *buf;
    };
    struct clos {
      file_id fid;
    };
    struct size {
      file_id fid;
    };
    struct seen {
      file_id fid;
      int pos;
    };
    struct chld {
      int pid;
      int fd;
    };
    struct gpic{
      char *path;
      int type, page;
    };
    struct spic {
      char *path;
      pic_cache cache;
    };

#ifdef __cplusplus

    using dataa = std::variant<open, read, writ, clos, size, seen, chld, gpic, spic>;

    struct data : public std::variant<open, read, writ, clos, size, seen, chld, gpic, spic> {
        public:
        int time;
        message to_enum();
        void log(FILE *f);
        data(int time, dataa param): query::dataa(param), time(time) {};
    };
#endif
}

/* ANSWERS */

enum accs_answer {
  ACCS_PASS = 0,
  ACCS_OK   = 1,
  ACCS_ENOENT = 2,
  ACCS_EACCES = 3,
};

struct stat_time {
  uint32_t sec, nsec;
};

struct stat_answer {
  uint32_t dev, ino;
  uint32_t mode;
  uint32_t nlink;
  uint32_t uid, gid;
  uint32_t rdev;
  uint32_t size;
  uint32_t blksize, blocks;
  struct stat_time atime, ctime, mtime;
};

#define READ_FORK (-1)

namespace answer {
    enum message : uint32_t {
        A_DONE = PACK('D','O','N','E'),
        A_PASS = PACK('P','A','S','S'),
        A_SIZE = PACK('S','I','Z','E'),
        A_READ = PACK('R','E','A','D'),
        A_FORK = PACK('F','O','R','K'),
        A_OPEN = PACK('O','P','E','N'),
        A_GPIC = PACK('G','P','I','C'),
    };

    struct size {
      int size;
    };
    struct read {
      int size;
    };
    struct open {
      int size;
    };
    struct gpic {
      float bounds[4];
    };
    struct done {};
    struct pass {};
    struct fork {};

    using dataa = std::variant<open, read, size, gpic, done, pass, fork>;

    struct data : public dataa {
        data(dataa params): dataa(params) {};
        message to_enum() const;
        void log(FILE *f);
    };
}
/* "ASK" :P */

enum ask {
  C_FLSH = PACK('F','L','S','H'),
};

typedef struct {
  enum ask tag;
  union {
    struct {
      int pid;
    } term;
    struct {
      int fid, pos;
    } fenc;
    struct {
      int fid;
    } flsh;
  };
} ask_t;

/* Channel */

struct Channel
{
  struct {
    char buffer[BUF_SIZE];
    int pos, len;
  } input;
  struct {
    char buffer[BUF_SIZE];
    int pos;
  } output;
  int passed_fd;
  char *buf;
  int buf_size;
  Channel();
  ~Channel();
  bool handshake(int fd);
  bool has_pending_query(int fd, int timeout);
  std::optional<query::data> read_query(int fd);
  query::message peek_query(int fd);
  void write_ask(int fd, ask_t *a);
  void write_answer(int fd, const answer::data &a);
  void *get_buffer(size_t n);
  void flush(int fd);
  void reset();

private:
  ssize_t read_(int fd, void *data, size_t len);
  bool read_all(int fd, char *buf, int size);
  void cflush(int fd);
  bool refill_at_least(int fd, int at_least);
  void resize_buf();
  int cgetc(int fd);
  int read_zstr(int fd, int *pos);
  bool read_bytes(int fd, int pos, int size);
  void write_bytes(int fd, void *buf, int size);
  bool try_read_u32(int fd, uint32_t *tag);
  uint32_t read_u32(int fd);
  void write_u32(int fd, uint32_t u);
  float read_f32(int fd);
  void write_f32(int fd, float f);
};

#endif /*!SPROTOCOL_H*/
