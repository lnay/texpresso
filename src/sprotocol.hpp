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

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdint.h>
#include "myabort.h"

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

struct pic_cache {
  int type, page;
  float bounds[4];
};

#ifdef __cplusplus
#include <variant>
#include <optional>

namespace query {
    enum message : uint32_t {
      OPEN = PACK('O','P','E','N'),
      READ = PACK('R','E','A','D'),
      WRIT = PACK('W','R','I','T'),
      CLOS = PACK('C','L','O','S'),
      SIZE = PACK('S','I','Z','E'),
      SEEN = PACK('S','E','E','N'),
      GPIC = PACK('G','P','I','C'),
      SPIC = PACK('S','P','I','C'),
      CHLD = PACK('C','H','L','D'),
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
      struct pic_cache cache;
    };

    using dataa = std::variant<open, read, writ, clos, size, seen, chld, gpic, spic>;

    struct data : public std::variant<open, read, writ, clos, size, seen, chld, gpic, spic> {
        public:
        int time;
        message to_enum();
        void log(FILE *f);
        data(int time, dataa param): query::dataa(param), time(time) {};
    };
}

/* ANSWERS */

namespace answer {
    enum message : uint32_t {
        DONE = PACK('D','O','N','E'),
        PASS = PACK('P','A','S','S'),
        SIZE = PACK('S','I','Z','E'),
        READ = PACK('R','E','A','D'),
        FORK = PACK('F','O','R','K'),
        OPEN = PACK('O','P','E','N'),
        GPIC = PACK('G','P','I','C'),
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

enum accs_answer {
  ACCS_PASS = 0,
  ACCS_OK   = 1,
  ACCS_ENOENT = 2,
  ACCS_EACCES = 3,
};

namespace status {
    struct time {
        uint32_t sec, nsec;
    };

    struct answer {
        uint32_t dev, ino;
        uint32_t mode;
        uint32_t nlink;
        uint32_t uid, gid;
        uint32_t rdev;
        uint32_t size;
        uint32_t blksize, blocks;
        struct time atime, ctime, mtime;
    };
}

#define READ_FORK (-1)


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

/* channel */

class Channel
{
  struct {
    char buffer[BUF_SIZE]; // replace with string?
    int pos, len;
  } input;
  struct {
    char buffer[BUF_SIZE]; // replace with string?
    int pos;
  } output;
  int passed_fd;
  char *buf; // replace with string?
  int buf_size;

  ssize_t read_(int fd, void *data, size_t len);
  int buffered_read_at_least(int fd, char *buf, int atleast, int size);
  bool read_all(int fd, char *buf, int size);
  void cflush(int fd);
  bool refill_at_least(int fd, int at_least);
  void resize_buf();
  int cgetc(int fd);
  int read_zstr(int fd, int *pos);
  bool read_bytes(int fd, int pos, int size);
  void write_bytes(int fd, void *buf, int size);
  void write_time(int fd, struct status::time tm);

  public:
  void flush(int fd);
  void reset();
  std::optional<query::data> read_query(int fd);
  void write_answer(int fd, const answer::data &a);
  void write_ask(int fd, ask_t *a);
  bool has_pending_query(int fd, int timeout);
  bool handshake(int fd);
  std::optional<query::message> peek(int fd);
  void *get_buffer(size_t n);
  Channel();
  ~Channel();

  template<typename T> friend std::optional<T> read(Channel *t, int fd);
  template<typename T> friend void write(Channel *t, int fd, T value);
};

#endif /*!__cplusplus*/
#endif /*!SPROTOCOL_H*/
