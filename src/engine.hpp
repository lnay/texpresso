#ifndef GENERIC_ENGINE_H_
#define GENERIC_ENGINE_H_
#include "state.h"
// needed by TexEngine:
#include "sprotocol.h"
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


#include "incdvi.h"
#include "synctex.h"

typedef enum {
  DOC_RUNNING,
  DOC_TERMINATED
} txp_engine_status;

namespace txp {
class Engine
{
public:
  virtual ~Engine() = default;
  virtual bool step(bool restart_if_needed) = 0;
  virtual void begin_changes() = 0;
  virtual void detect_changes() = 0;
  virtual bool end_changes() = 0;
  virtual int page_count() = 0;
  virtual fz_display_list *render_page(int page) = 0;
  virtual txp_engine_status get_status() = 0;
  virtual float scale_factor() = 0;
  virtual synctex_t *synctex(fz_buffer **buf) = 0;
  virtual fileentry_t *find_file(const char *path) = 0;
  virtual void notify_file_changes(fileentry_t *entry, int offset) = 0;
};


typedef struct
{
  int pid, fd;
  int trace_len;
  mark_t snap;
} process_t;
typedef struct
{
  fileentry_t *entry;
  int position;
} fence_t;
typedef struct
{
  fileentry_t *entry;
  int seen, time;
} trace_entry_t;

class TexEngine : public Engine
{
public:
  fz_context &ctx;
  TexEngine(fz_context &ctx,
            const char *tectonic_path,
            const char *inclusion_path,
            const char *tex_dir,
            const char *tex_name);
  ~TexEngine();
  bool step(bool restart_if_needed) override;
  void begin_changes() override;
  void detect_changes() override;
  bool end_changes() override;
  int page_count() override;
  fz_display_list *render_page(int page) override;
  txp_engine_status get_status() override;
  float scale_factor() override;
  synctex_t *synctex(fz_buffer **buf) override;
  fileentry_t *find_file(const char *path) override;
  void notify_file_changes(fileentry_t *entry, int offset) override;
// private:
  char *name;
  char *tectonic_path;
  char *inclusion_path;
  filesystem_t *fs;
  state_t st;
  log_t *log;

  Channel *c;
  process_t processes[32];
  int process_count;

  trace_entry_t *trace;
  int trace_cap;
  fence_t fences[16];
  int fence_pos;
  mark_t restart;

  bundle_server *bundle;
  incdvi_t *dvi;
  synctex_t *stex;

  struct {
    int trace_len, offset, flush;
  } rollback;
};
// class PDFEngine : Engine
// {
//   fz_context &ctx;
//   PDFEngine(fz_context *ctx, const char *pdf_path);
//   ~PDFEngine();
// };
// class DVIEngine : Engine
// {
//   fz_context &ctx;
//   DVIEngine(fz_context *ctx,
//             const char *tectonic_path,
//             const char *dvi_dir,
//             const char *dvi_path);
//   ~DVIEngine();
// };
}

#endif // GENERIC_ENGINE_H_
