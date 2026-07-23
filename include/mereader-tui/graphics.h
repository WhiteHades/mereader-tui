#pragma once

#include "mereader-tui/config.h"
#include "mereader-tui/document.h"

/* Hard resource and geometry limits applied before decoder or renderer loops. */
#define MEREADER_TUI_GRAPHICS_DEFAULT_CACHE_BYTES (32U * 1024U * 1024U)
#define MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES (16U * 1024U * 1024U)
#define MEREADER_TUI_GRAPHICS_MAX_TRANSMIT_BYTES (16U * 1024U * 1024U)
#define MEREADER_TUI_GRAPHICS_MAX_SOURCE_DIMENSION 32768
#define MEREADER_TUI_GRAPHICS_MAX_SOURCE_PIXELS 64000000U
#define MEREADER_TUI_GRAPHICS_MAX_RENDER_PIXELS 10000000U
#define MEREADER_TUI_GRAPHICS_MAX_COLUMNS 1024
#define MEREADER_TUI_GRAPHICS_MAX_ROWS 1024
#define MEREADER_TUI_GRAPHICS_MAX_VIEWPORT_DIMENSION 32767
#define MEREADER_TUI_GRAPHICS_MAX_OCCLUSIONS 8U
#define MEREADER_TUI_GRAPHICS_MAX_VISIBLE_RECTS 64U
#define MEREADER_TUI_GRAPHICS_MAX_PROBES 1024U
#define MEREADER_TUI_GRAPHICS_MAX_PROBE_BYTES (64U * 1024U * 1024U)
#define MEREADER_TUI_GRAPHICS_MAX_FRAMES 1U
#define MEREADER_TUI_GRAPHICS_KITTY_CHUNK_BYTES 4096U
#define MEREADER_TUI_GRAPHICS_KITTY_SCREEN_SEQUENCE_BYTES 768U

typedef enum MereaderTuiGraphicsMultiplexer : uint8_t {
  MEREADER_TUI_GRAPHICS_MULTIPLEXER_NONE = 0,
  MEREADER_TUI_GRAPHICS_MULTIPLEXER_TMUX,
  MEREADER_TUI_GRAPHICS_MULTIPLEXER_SCREEN,
} MereaderTuiGraphicsMultiplexer;

typedef struct MereaderTuiGraphicsEnvironment {
  const char *term_program;
  const char *term;
  const char *kitty_window_id;
  const char *colorterm;
  const char *tmux;
  const char *sty;
  bool colors;
  bool output_is_tty;
} MereaderTuiGraphicsEnvironment;

typedef struct MereaderTuiGraphicsRect {
  int row;
  int column;
  int rows;
  int columns;
} MereaderTuiGraphicsRect;

typedef struct MereaderTuiGraphicsPlacement {
  int row;
  int column;
  int viewport_rows;
  int viewport_columns;
  const MereaderTuiGraphicsRect *occlusions;
  size_t occlusion_count;
} MereaderTuiGraphicsPlacement;

typedef struct MereaderTuiGraphicsSurface {
  const unsigned char *pixels;
  size_t pixel_bytes;
  int width;
  int height;
  int columns;
  int rows;
  int rowstride;
  uint32_t image_id;
  void *backing;
} MereaderTuiGraphicsSurface;

typedef struct MereaderTuiGraphicsCacheStats {
  size_t entries;
  size_t bytes;
  size_t maximum_bytes;
  uint64_t generation;
} MereaderTuiGraphicsCacheStats;

typedef struct MereaderTuiGraphicsContext MereaderTuiGraphicsContext;

typedef bool (*MereaderTuiGraphicsWriter)(void *user_data, const void *data,
                                   size_t length);
typedef bool (*MereaderTuiGraphicsCellWriter)(void *user_data, int row, int column,
                                       uint32_t foreground,
                                       uint32_t background);

[[nodiscard]] MereaderTuiGraphicsMultiplexer
mereader_tui_graphics_multiplexer(const MereaderTuiGraphicsEnvironment *environment);
[[nodiscard]] bool
mereader_tui_graphics_truecolor_available(const MereaderTuiGraphicsEnvironment *environment);
[[nodiscard]] MereaderTuiImageMode
mereader_tui_graphics_select_mode(MereaderTuiImageMode configured, bool explicit_mode,
                          const MereaderTuiGraphicsEnvironment *environment);
[[nodiscard]] bool mereader_tui_graphics_probe_resource(const MereaderTuiResource *resource,
                                                int *width, int *height,
                                                MereaderTuiError *error);
void mereader_tui_graphics_composite_pixel(uint32_t background,
                                   const unsigned char source[4],
                                   unsigned char output[3]);

[[nodiscard]] MereaderTuiGraphicsContext *
mereader_tui_graphics_create(size_t maximum_bytes, MereaderTuiGraphicsMultiplexer multiplexer,
                     uint32_t background, MereaderTuiError *error);
void mereader_tui_graphics_free(MereaderTuiGraphicsContext *context);
[[nodiscard]] bool mereader_tui_graphics_resize(MereaderTuiGraphicsContext *context,
                                         int columns, int rows,
                                         MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_graphics_set_cell_pixels(MereaderTuiGraphicsContext *context,
                                                  int width, int height,
                                                  MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_graphics_set_background(MereaderTuiGraphicsContext *context,
                                                uint32_t background,
                                                MereaderTuiError *error);
[[nodiscard]] MereaderTuiGraphicsCacheStats
mereader_tui_graphics_cache_stats(const MereaderTuiGraphicsContext *context);
/* surface must be zero-initialized or released before prepare; it then owns a
 * backing reference until surface_release. Do not copy an owning surface. */
[[nodiscard]] bool mereader_tui_graphics_prepare(MereaderTuiGraphicsContext *context,
                                         MereaderTuiDocument *document,
                                         size_t block_index, int columns,
                                         int rows, MereaderTuiGraphicsSurface *surface,
                                         MereaderTuiError *error);
void mereader_tui_graphics_surface_release(MereaderTuiGraphicsSurface *surface);

[[nodiscard]] bool mereader_tui_graphics_render_ansi(
    const MereaderTuiGraphicsSurface *surface, const MereaderTuiGraphicsPlacement *placement,
    MereaderTuiGraphicsWriter writer, void *user_data, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_graphics_render_cells(
    const MereaderTuiGraphicsSurface *surface, const MereaderTuiGraphicsPlacement *placement,
    MereaderTuiGraphicsCellWriter writer, void *user_data, MereaderTuiError *error);
[[nodiscard]] unsigned mereader_tui_graphics_rgb_to_256(uint32_t rgb);
[[nodiscard]] unsigned mereader_tui_graphics_rgb_to_palette(uint32_t rgb,
                                                    unsigned colors);
[[nodiscard]] short mereader_tui_graphics_pair(MereaderTuiGraphicsContext *context,
                                       unsigned foreground, unsigned background,
                                       short first_pair, short pair_capacity,
                                       bool *created);
[[nodiscard]] size_t
mereader_tui_graphics_pair_count(const MereaderTuiGraphicsContext *context);

/* Kitty calls retain writer/user_data for eviction and invalidation deletes;
 * keep them valid until delete_all succeeds or the context is freed. */
[[nodiscard]] bool
mereader_tui_graphics_kitty_transmit(MereaderTuiGraphicsContext *context, uint32_t image_id,
                             const unsigned char *png, size_t png_length,
                             MereaderTuiGraphicsWriter writer, void *user_data,
                             MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_graphics_kitty_place(
    MereaderTuiGraphicsContext *context, const MereaderTuiGraphicsSurface *surface,
    const MereaderTuiGraphicsPlacement *placement, MereaderTuiGraphicsWriter writer,
    void *user_data, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_graphics_kitty_draw(
    MereaderTuiGraphicsContext *context, const MereaderTuiGraphicsSurface *surface,
    const MereaderTuiGraphicsPlacement *placement, MereaderTuiGraphicsWriter writer,
    void *user_data, MereaderTuiError *error);
[[nodiscard]] bool
mereader_tui_graphics_kitty_delete_placements(MereaderTuiGraphicsContext *context,
                                      MereaderTuiGraphicsWriter writer,
                                      void *user_data, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_graphics_kitty_delete_all(MereaderTuiGraphicsContext *context,
                                                  MereaderTuiGraphicsWriter writer,
                                                  void *user_data,
                                                  MereaderTuiError *error);
