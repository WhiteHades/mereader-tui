#pragma once

#include "baca/config.h"
#include "baca/document.h"

/* Hard resource and geometry limits applied before decoder or renderer loops. */
#define BACA_GRAPHICS_DEFAULT_CACHE_BYTES (32U * 1024U * 1024U)
#define BACA_GRAPHICS_MAX_INPUT_BYTES (16U * 1024U * 1024U)
#define BACA_GRAPHICS_MAX_TRANSMIT_BYTES (16U * 1024U * 1024U)
#define BACA_GRAPHICS_MAX_SOURCE_DIMENSION 32768
#define BACA_GRAPHICS_MAX_SOURCE_PIXELS 64000000U
#define BACA_GRAPHICS_MAX_COLUMNS 1024
#define BACA_GRAPHICS_MAX_ROWS 1024
#define BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION 32767
#define BACA_GRAPHICS_MAX_OCCLUSIONS 8U
#define BACA_GRAPHICS_MAX_VISIBLE_RECTS 64U
#define BACA_GRAPHICS_MAX_PROBES 1024U
#define BACA_GRAPHICS_MAX_PROBE_BYTES (64U * 1024U * 1024U)
#define BACA_GRAPHICS_MAX_FRAMES 1U
#define BACA_GRAPHICS_KITTY_CHUNK_BYTES 4096U
#define BACA_GRAPHICS_KITTY_SCREEN_SEQUENCE_BYTES 768U

typedef enum BacaGraphicsMultiplexer : uint8_t {
  BACA_GRAPHICS_MULTIPLEXER_NONE = 0,
  BACA_GRAPHICS_MULTIPLEXER_TMUX,
  BACA_GRAPHICS_MULTIPLEXER_SCREEN,
} BacaGraphicsMultiplexer;

typedef struct BacaGraphicsEnvironment {
  const char *term_program;
  const char *term;
  const char *kitty_window_id;
  const char *colorterm;
  const char *tmux;
  const char *sty;
  bool colors;
  bool output_is_tty;
} BacaGraphicsEnvironment;

typedef struct BacaGraphicsRect {
  int row;
  int column;
  int rows;
  int columns;
} BacaGraphicsRect;

typedef struct BacaGraphicsPlacement {
  int row;
  int column;
  int viewport_rows;
  int viewport_columns;
  const BacaGraphicsRect *occlusions;
  size_t occlusion_count;
} BacaGraphicsPlacement;

typedef struct BacaGraphicsSurface {
  const unsigned char *pixels;
  size_t pixel_bytes;
  int width;
  int height;
  int rowstride;
  uint32_t image_id;
  void *backing;
} BacaGraphicsSurface;

typedef struct BacaGraphicsCacheStats {
  size_t entries;
  size_t bytes;
  size_t maximum_bytes;
  uint64_t generation;
} BacaGraphicsCacheStats;

typedef struct BacaGraphicsContext BacaGraphicsContext;

typedef bool (*BacaGraphicsWriter)(void *user_data, const void *data,
                                   size_t length);
typedef bool (*BacaGraphicsCellWriter)(void *user_data, int row, int column,
                                       uint32_t foreground,
                                       uint32_t background);

[[nodiscard]] BacaGraphicsMultiplexer
baca_graphics_multiplexer(const BacaGraphicsEnvironment *environment);
[[nodiscard]] bool
baca_graphics_truecolor_available(const BacaGraphicsEnvironment *environment);
[[nodiscard]] BacaImageMode
baca_graphics_select_mode(BacaImageMode configured, bool explicit_mode,
                          const BacaGraphicsEnvironment *environment);
[[nodiscard]] bool baca_graphics_probe_resource(const BacaResource *resource,
                                                int *width, int *height,
                                                BacaError *error);
void baca_graphics_composite_pixel(uint32_t background,
                                   const unsigned char source[4],
                                   unsigned char output[3]);

[[nodiscard]] BacaGraphicsContext *
baca_graphics_create(size_t maximum_bytes, BacaGraphicsMultiplexer multiplexer,
                     uint32_t background, BacaError *error);
void baca_graphics_free(BacaGraphicsContext *context);
[[nodiscard]] bool baca_graphics_resize(BacaGraphicsContext *context,
                                        int columns, int rows,
                                        BacaError *error);
[[nodiscard]] bool baca_graphics_set_background(BacaGraphicsContext *context,
                                                uint32_t background,
                                                BacaError *error);
[[nodiscard]] BacaGraphicsCacheStats
baca_graphics_cache_stats(const BacaGraphicsContext *context);
/* surface must be zero-initialized or released before prepare; it then owns a
 * backing reference until surface_release. Do not copy an owning surface. */
[[nodiscard]] bool baca_graphics_prepare(BacaGraphicsContext *context,
                                         BacaDocument *document,
                                         size_t block_index, int columns,
                                         int rows, BacaGraphicsSurface *surface,
                                         BacaError *error);
void baca_graphics_surface_release(BacaGraphicsSurface *surface);

[[nodiscard]] bool baca_graphics_render_ansi(
    const BacaGraphicsSurface *surface, const BacaGraphicsPlacement *placement,
    BacaGraphicsWriter writer, void *user_data, BacaError *error);
[[nodiscard]] bool baca_graphics_render_cells(
    const BacaGraphicsSurface *surface, const BacaGraphicsPlacement *placement,
    BacaGraphicsCellWriter writer, void *user_data, BacaError *error);
[[nodiscard]] unsigned baca_graphics_rgb_to_256(uint32_t rgb);
[[nodiscard]] unsigned baca_graphics_rgb_to_palette(uint32_t rgb,
                                                    unsigned colors);
[[nodiscard]] short baca_graphics_pair(BacaGraphicsContext *context,
                                       unsigned foreground, unsigned background,
                                       short first_pair, short pair_capacity,
                                       bool *created);
[[nodiscard]] size_t
baca_graphics_pair_count(const BacaGraphicsContext *context);

/* Kitty calls retain writer/user_data for eviction and invalidation deletes;
 * keep them valid until delete_all succeeds or the context is freed. */
[[nodiscard]] bool
baca_graphics_kitty_transmit(BacaGraphicsContext *context, uint32_t image_id,
                             const unsigned char *png, size_t png_length,
                             BacaGraphicsWriter writer, void *user_data,
                             BacaError *error);
[[nodiscard]] bool baca_graphics_kitty_place(
    BacaGraphicsContext *context, const BacaGraphicsSurface *surface,
    const BacaGraphicsPlacement *placement, BacaGraphicsWriter writer,
    void *user_data, BacaError *error);
[[nodiscard]] bool baca_graphics_kitty_draw(
    BacaGraphicsContext *context, const BacaGraphicsSurface *surface,
    const BacaGraphicsPlacement *placement, BacaGraphicsWriter writer,
    void *user_data, BacaError *error);
[[nodiscard]] bool
baca_graphics_kitty_delete_placements(BacaGraphicsContext *context,
                                      BacaGraphicsWriter writer,
                                      void *user_data, BacaError *error);
[[nodiscard]] bool baca_graphics_kitty_delete_all(BacaGraphicsContext *context,
                                                  BacaGraphicsWriter writer,
                                                  void *user_data,
                                                  BacaError *error);
