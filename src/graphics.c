#include "baca/graphics.h"

#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <gdk-pixbuf/gdk-pixbuf.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

enum {
  BACA_GRAPHICS_PAIR_CACHE_LIMIT = 256,
  BACA_GRAPHICS_PNG_RAW_CHUNK = 3072,
  BACA_GRAPHICS_SCREEN_PNG_RAW_CHUNK = 480,
  BACA_GRAPHICS_CONTEXT_ID_SPAN = 131072,
  BACA_GRAPHICS_MAX_REMOTE_IMAGES = BACA_GRAPHICS_MAX_PROBES * 2,
  BACA_GRAPHICS_MAX_REMOTE_PLACEMENTS = BACA_GRAPHICS_MAX_PROBES * 8,
};

typedef struct BacaGraphicsCacheEntry {
  GdkPixbuf *pixbuf;
  char *uri;
  const BacaDocument *document;
  uint64_t document_instance_id;
  size_t bytes;
  uint64_t last_used;
  uint32_t image_id;
  int columns;
  int rows;
  bool transmitted;
} BacaGraphicsCacheEntry;

typedef struct BacaGraphicsRemotePlacement {
  uint32_t image_id;
  uint32_t placement_id;
} BacaGraphicsRemotePlacement;

typedef struct BacaGraphicsPairEntry {
  unsigned foreground;
  unsigned background;
  short pair;
} BacaGraphicsPairEntry;

struct BacaGraphicsContext {
  BacaGraphicsCacheEntry *entries;
  size_t entry_count;
  size_t entry_capacity;
  size_t bytes;
  size_t maximum_bytes;
  uint64_t tick;
  uint64_t generation;
  uint32_t image_id_base;
  uint32_t image_id_offset;
  uint32_t next_placement_id;
  uint32_t background;
  int terminal_columns;
  int terminal_rows;
  int cell_pixel_width;
  int cell_pixel_height;
  BacaGraphicsMultiplexer multiplexer;
  uint32_t *remote_images;
  size_t remote_image_count;
  size_t remote_image_capacity;
  BacaGraphicsRemotePlacement *remote_placements;
  size_t remote_placement_count;
  size_t remote_placement_capacity;
  BacaGraphicsWriter writer;
  void *writer_data;
  BacaGraphicsPairEntry pairs[BACA_GRAPHICS_PAIR_CACHE_LIMIT];
  size_t pair_count;
};

typedef struct BacaProbeState {
  int width;
  int height;
  int target_width;
  int target_height;
  bool invalid;
} BacaProbeState;

static atomic_uint_least32_t graphics_id_seed;
static atomic_uint_least32_t graphics_context_serial;

static bool graphics_nonempty(const char *value) {
  return value != NULL && value[0] != '\0';
}

static bool graphics_case_contains(const char *value, const char *needle) {
  if (value == NULL || needle == NULL || needle[0] == '\0') {
    return false;
  }
  const size_t needle_length = strlen(needle);
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    size_t index = 0U;
    while (index < needle_length && cursor[index] != '\0') {
      unsigned char left = (unsigned char)cursor[index];
      unsigned char right = (unsigned char)needle[index];
      if (left >= 'A' && left <= 'Z') {
        left = (unsigned char)(left + ('a' - 'A'));
      }
      if (right >= 'A' && right <= 'Z') {
        right = (unsigned char)(right + ('a' - 'A'));
      }
      if (left != right) {
        break;
      }
      ++index;
    }
    if (index == needle_length) {
      return true;
    }
  }
  return false;
}

BacaGraphicsMultiplexer
baca_graphics_multiplexer(const BacaGraphicsEnvironment *environment) {
  if (environment == NULL) {
    return BACA_GRAPHICS_MULTIPLEXER_NONE;
  }
  if (graphics_nonempty(environment->tmux) ||
      graphics_case_contains(environment->term, "tmux")) {
    return BACA_GRAPHICS_MULTIPLEXER_TMUX;
  }
  if (graphics_nonempty(environment->sty) ||
      graphics_case_contains(environment->term, "screen")) {
    return BACA_GRAPHICS_MULTIPLEXER_SCREEN;
  }
  return BACA_GRAPHICS_MULTIPLEXER_NONE;
}

bool baca_graphics_truecolor_available(
    const BacaGraphicsEnvironment *environment) {
  if (environment == NULL || !environment->colors) {
    return false;
  }
  return graphics_case_contains(environment->colorterm, "truecolor") ||
         graphics_case_contains(environment->colorterm, "24bit") ||
         graphics_case_contains(environment->term, "direct") ||
         graphics_case_contains(environment->term, "truecolor") ||
         graphics_case_contains(environment->term_program, "ghostty") ||
         graphics_case_contains(environment->term_program, "kitty") ||
         graphics_case_contains(environment->term, "kitty") ||
         graphics_nonempty(environment->kitty_window_id);
}

BacaImageMode
baca_graphics_select_mode(BacaImageMode configured, bool explicit_mode,
                          const BacaGraphicsEnvironment *environment) {
  if (configured == BACA_IMAGE_MODE_PLACEHOLDER || environment == NULL ||
      !environment->output_is_tty) {
    return BACA_IMAGE_MODE_PLACEHOLDER;
  }
  const BacaGraphicsMultiplexer multiplexer =
      baca_graphics_multiplexer(environment);
  if (configured == BACA_IMAGE_MODE_KITTY) {
    if (multiplexer != BACA_GRAPHICS_MULTIPLEXER_NONE && !explicit_mode) {
      return environment->colors ? BACA_IMAGE_MODE_ANSI
                                 : BACA_IMAGE_MODE_PLACEHOLDER;
    }
    return BACA_IMAGE_MODE_KITTY;
  }
  if (configured == BACA_IMAGE_MODE_ANSI) {
    return environment->colors ? BACA_IMAGE_MODE_ANSI
                               : BACA_IMAGE_MODE_PLACEHOLDER;
  }

  const bool kitty =
      graphics_case_contains(environment->term_program, "ghostty") ||
      graphics_case_contains(environment->term_program, "kitty") ||
      graphics_case_contains(environment->term, "kitty") ||
      graphics_nonempty(environment->kitty_window_id);
  if (kitty && multiplexer == BACA_GRAPHICS_MULTIPLEXER_NONE) {
    return BACA_IMAGE_MODE_KITTY;
  }
  return environment->colors ? BACA_IMAGE_MODE_ANSI
                             : BACA_IMAGE_MODE_PLACEHOLDER;
}

static GdkPixbufLoader *graphics_loader_new(const char *mime_type,
                                             GError **gerror) {
  if (graphics_nonempty(mime_type)) {
    GdkPixbufLoader *loader =
        gdk_pixbuf_loader_new_with_mime_type(mime_type, gerror);
    if (loader != NULL) {
      return loader;
    }
    g_clear_error(gerror);
  }
  return gdk_pixbuf_loader_new();
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static bool graphics_loader_is_animated(GdkPixbufLoader *loader) {
  GdkPixbufAnimation *animation = gdk_pixbuf_loader_get_animation(loader);
  return animation != NULL &&
         !gdk_pixbuf_animation_is_static_image(animation);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void graphics_probe_size(GdkPixbufLoader *loader, int width, int height,
                                 gpointer user_data) {
  BacaProbeState *state = user_data;
  if (state->width == 0 && state->height == 0) {
    state->width = width;
    state->height = height;
  }
  if (width <= 0 || height <= 0 || width > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
      height > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
      (size_t)width > BACA_GRAPHICS_MAX_SOURCE_PIXELS / (size_t)height) {
    state->invalid = true;
  }
  if (state->target_width > 0 && state->target_height > 0) {
    gdk_pixbuf_loader_set_size(loader, state->target_width,
                               state->target_height);
  }
}

static bool graphics_skip_gif_subblocks(const unsigned char *data,
                                        size_t length, size_t *offset) {
  while (*offset < length) {
    const size_t block = data[(*offset)++];
    if (block == 0U) {
      return true;
    }
    if (block > length - *offset) {
      return false;
    }
    *offset += block;
  }
  return false;
}

static bool graphics_gif_has_multiple_frames(const unsigned char *data,
                                             size_t length) {
  if (length < 13U ||
      (memcmp(data, "GIF87a", 6U) != 0 && memcmp(data, "GIF89a", 6U) != 0)) {
    return false;
  }
  size_t offset = 13U;
  if ((data[10] & 0x80U) != 0U) {
    const size_t table = 3U << ((data[10] & 0x07U) + 1U);
    if (table > length - offset) {
      return false;
    }
    offset += table;
  }
  unsigned frames = 0U;
  while (offset < length) {
    const unsigned marker = data[offset++];
    if (marker == 0x3bU) {
      return false;
    }
    if (marker == 0x21U) {
      if (offset >= length) {
        return false;
      }
      ++offset;
      if (!graphics_skip_gif_subblocks(data, length, &offset)) {
        return false;
      }
      continue;
    }
    if (marker != 0x2cU || length - offset < 9U) {
      return false;
    }
    ++frames;
    if (frames > BACA_GRAPHICS_MAX_FRAMES) {
      return true;
    }
    const unsigned packed = data[offset + 8U];
    offset += 9U;
    if ((packed & 0x80U) != 0U) {
      const size_t table = 3U << ((packed & 0x07U) + 1U);
      if (table > length - offset) {
        return false;
      }
      offset += table;
    }
    if (offset >= length) {
      return false;
    }
    ++offset;
    if (!graphics_skip_gif_subblocks(data, length, &offset)) {
      return false;
    }
  }
  return false;
}

static uint32_t graphics_read_be32(const unsigned char *data) {
  return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
         ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static uint32_t graphics_read_le32(const unsigned char *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
         ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static bool graphics_png_has_multiple_frames(const unsigned char *data,
                                             size_t length) {
  static const unsigned char signature[] = {0x89U, 'P', 'N', 'G',
                                             '\r',  '\n', 0x1aU, '\n'};
  if (length < sizeof(signature) ||
      memcmp(data, signature, sizeof(signature)) != 0) {
    return false;
  }
  size_t offset = sizeof(signature);
  while (length - offset >= 12U) {
    const size_t chunk_length = graphics_read_be32(data + offset);
    if (chunk_length > length - offset - 12U) {
      return false;
    }
    if (memcmp(data + offset + 4U, "acTL", 4U) == 0 &&
        chunk_length >= 8U &&
        graphics_read_be32(data + offset + 8U) >
            BACA_GRAPHICS_MAX_FRAMES) {
      return true;
    }
    offset += chunk_length + 12U;
  }
  return false;
}

static bool graphics_webp_has_multiple_frames(const unsigned char *data,
                                              size_t length) {
  if (length < 12U || memcmp(data, "RIFF", 4U) != 0 ||
      memcmp(data + 8U, "WEBP", 4U) != 0) {
    return false;
  }
  size_t offset = 12U;
  while (length - offset >= 8U) {
    const size_t chunk_length = graphics_read_le32(data + offset + 4U);
    if (chunk_length > length - offset - 8U) {
      return false;
    }
    if (memcmp(data + offset, "ANIM", 4U) == 0 ||
        memcmp(data + offset, "ANMF", 4U) == 0) {
      return true;
    }
    const size_t padding = chunk_length & 1U;
    if (chunk_length > SIZE_MAX - padding ||
        chunk_length + padding > length - offset - 8U) {
      return false;
    }
    offset += 8U + chunk_length + padding;
  }
  return false;
}

static bool graphics_resource_has_multiple_frames(const BacaResource *resource) {
  return graphics_gif_has_multiple_frames(resource->data, resource->length) ||
         graphics_png_has_multiple_frames(resource->data, resource->length) ||
         graphics_webp_has_multiple_frames(resource->data, resource->length);
}

bool baca_graphics_probe_resource(const BacaResource *resource, int *width,
                                  int *height, BacaError *error) {
  if (width != NULL) {
    *width = 0;
  }
  if (height != NULL) {
    *height = 0;
  }
  if (resource == NULL || width == NULL || height == NULL ||
      resource->data == NULL || resource->length == 0U) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "image resource and dimension outputs are required");
    return false;
  }
  if (resource->length > BACA_GRAPHICS_MAX_INPUT_BYTES) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "compressed image exceeds the supported size limit");
    return false;
  }
  if (graphics_resource_has_multiple_frames(resource)) {
    baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                   "animated images are not rendered");
    return false;
  }

  GError *gerror = NULL;
  GdkPixbufLoader *loader = graphics_loader_new(resource->mime_type, &gerror);
  if (loader == NULL) {
    baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                   "no GdkPixbuf loader for image: %s",
                   gerror == NULL ? "unknown format" : gerror->message);
    g_clear_error(&gerror);
    return false;
  }
  BacaProbeState state = {
      .target_width = 2,
      .target_height = 2,
  };
  (void)g_signal_connect(loader, "size-prepared",
                         G_CALLBACK(graphics_probe_size), &state);
  const bool wrote =
      gdk_pixbuf_loader_write(loader, resource->data, resource->length,
                              &gerror) != FALSE;
  const bool closed =
      wrote && gdk_pixbuf_loader_close(loader, &gerror) != FALSE;
  const bool animated = closed && graphics_loader_is_animated(loader);
  g_object_unref(loader);
  if (state.invalid) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "image dimensions exceed the supported limits");
    g_clear_error(&gerror);
    return false;
  }
  if (animated) {
    baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                   "animated images are not rendered");
    g_clear_error(&gerror);
    return false;
  }
  if (!closed || state.width <= 0 || state.height <= 0) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "cannot decode image dimensions: %s",
                   gerror == NULL ? "invalid image data" : gerror->message);
    g_clear_error(&gerror);
    return false;
  }
  g_clear_error(&gerror);
  *width = state.width;
  *height = state.height;
  return true;
}

void baca_graphics_composite_pixel(uint32_t background,
                                   const unsigned char source[4],
                                   unsigned char output[3]) {
  const unsigned alpha = source[3];
  const unsigned inverse = 255U - alpha;
  const unsigned background_red = (background >> 16U) & 0xffU;
  const unsigned background_green = (background >> 8U) & 0xffU;
  const unsigned background_blue = background & 0xffU;
  output[0] = (unsigned char)(((unsigned)source[0] * alpha +
                               background_red * inverse + 127U) /
                              255U);
  output[1] = (unsigned char)(((unsigned)source[1] * alpha +
                               background_green * inverse + 127U) /
                              255U);
  output[2] = (unsigned char)(((unsigned)source[2] * alpha +
                               background_blue * inverse + 127U) /
                              255U);
}

static bool graphics_delete_remote_image(BacaGraphicsContext *context,
                                         uint32_t image_id,
                                         BacaGraphicsWriter writer,
                                         void *user_data, BacaError *error);
static bool graphics_delete_all_remote(BacaGraphicsContext *context,
                                       BacaGraphicsWriter writer,
                                       void *user_data, BacaError *error);
static size_t graphics_remote_image_index(const BacaGraphicsContext *context,
                                          uint32_t image_id);

static void graphics_entry_free(BacaGraphicsCacheEntry *entry) {
  if (entry->pixbuf != NULL) {
    g_object_unref(entry->pixbuf);
  }
  free(entry->uri);
  *entry = (BacaGraphicsCacheEntry){0};
}

static void graphics_cache_free_local(BacaGraphicsContext *context) {
  for (size_t index = 0U; index < context->entry_count; ++index) {
    graphics_entry_free(&context->entries[index]);
  }
  free(context->entries);
  context->entries = NULL;
  context->entry_count = 0U;
  context->entry_capacity = 0U;
  context->bytes = 0U;
  context->pair_count = 0U;
}

static bool graphics_cache_clear(BacaGraphicsContext *context,
                                 BacaError *error) {
  if (!graphics_delete_all_remote(context, context->writer,
                                  context->writer_data, error)) {
    return false;
  }
  graphics_cache_free_local(context);
  ++context->generation;
  return true;
}

static uint32_t graphics_process_id_seed(void) {
  uint_least32_t seed =
      atomic_load_explicit(&graphics_id_seed, memory_order_relaxed);
  if (seed != 0U) {
    return (uint32_t)seed;
  }
  uint_least32_t candidate = (uint_least32_t)g_random_int();
  if (candidate == 0U) {
    candidate = 1U;
  }
  uint_least32_t expected = 0U;
  if (!atomic_compare_exchange_strong_explicit(
          &graphics_id_seed, &expected, candidate, memory_order_relaxed,
          memory_order_relaxed)) {
    candidate = expected;
  }
  return (uint32_t)candidate;
}

BacaGraphicsContext *baca_graphics_create(size_t maximum_bytes,
                                          BacaGraphicsMultiplexer multiplexer,
                                          uint32_t background,
                                          BacaError *error) {
  if (multiplexer > BACA_GRAPHICS_MULTIPLEXER_SCREEN) {
    baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid graphics multiplexer");
    return NULL;
  }
  if (maximum_bytes == 0U) {
    maximum_bytes = BACA_GRAPHICS_DEFAULT_CACHE_BYTES;
  }
  BacaGraphicsContext *context = calloc(1U, sizeof(*context));
  if (context == NULL) {
    baca_error_set(error, BACA_ERROR_MEMORY,
                   "cannot allocate graphics context");
    return NULL;
  }
  context->maximum_bytes = maximum_bytes;
  context->multiplexer = multiplexer;
  context->background = background;
  context->cell_pixel_width = 1;
  context->cell_pixel_height = 2;
  context->generation = 1U;
  const uint_least32_t serial = atomic_fetch_add_explicit(
      &graphics_context_serial, 1U, memory_order_relaxed);
  context->image_id_base =
      graphics_process_id_seed() +
      (uint32_t)serial * (uint32_t)BACA_GRAPHICS_CONTEXT_ID_SPAN;
  context->next_placement_id = g_random_int();
  if (context->next_placement_id == 0U) {
    context->next_placement_id = 1U;
  }
  return context;
}

void baca_graphics_free(BacaGraphicsContext *context) {
  if (context == NULL) {
    return;
  }
  BacaError ignored = {0};
  if (!graphics_cache_clear(context, &ignored)) {
    graphics_cache_free_local(context);
  }
  free(context->remote_images);
  free(context->remote_placements);
  free(context);
}

bool baca_graphics_resize(BacaGraphicsContext *context, int columns, int rows,
                          BacaError *error) {
  if (context == NULL || columns < 0 || rows < 0) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid graphics terminal dimensions");
    return false;
  }
  if (columns > BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION) {
    columns = BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION;
  }
  if (rows > BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION) {
    rows = BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION;
  }
  if (context->terminal_columns == columns && context->terminal_rows == rows) {
    return true;
  }
  if (!graphics_cache_clear(context, error)) {
    return false;
  }
  context->terminal_columns = columns;
  context->terminal_rows = rows;
  return true;
}

bool baca_graphics_set_cell_pixels(BacaGraphicsContext *context, int width,
                                   int height, BacaError *error) {
  if (context == NULL || width <= 0 || height <= 0 ||
      width > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
      height > BACA_GRAPHICS_MAX_SOURCE_DIMENSION) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid terminal cell pixel dimensions");
    return false;
  }
  if (context->cell_pixel_width == width &&
      context->cell_pixel_height == height) {
    return true;
  }
  if (!graphics_cache_clear(context, error)) {
    return false;
  }
  context->cell_pixel_width = width;
  context->cell_pixel_height = height;
  return true;
}

bool baca_graphics_set_background(BacaGraphicsContext *context,
                                  uint32_t background, BacaError *error) {
  if (context == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT, "graphics context is required");
    return false;
  }
  if (context->background == background) {
    return true;
  }
  if (!graphics_cache_clear(context, error)) {
    return false;
  }
  context->background = background;
  return true;
}

BacaGraphicsCacheStats
baca_graphics_cache_stats(const BacaGraphicsContext *context) {
  if (context == NULL) {
    return (BacaGraphicsCacheStats){0};
  }
  return (BacaGraphicsCacheStats){
      .entries = context->entry_count,
      .bytes = context->bytes,
      .maximum_bytes = context->maximum_bytes,
      .generation = context->generation,
  };
}

static size_t graphics_pixbuf_bytes(GdkPixbuf *pixbuf) {
  const int height = gdk_pixbuf_get_height(pixbuf);
  const int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  if (height <= 0 || rowstride <= 0 ||
      (size_t)height > SIZE_MAX / (size_t)rowstride) {
    return SIZE_MAX;
  }
  return (size_t)height * (size_t)rowstride;
}

static BacaGraphicsCacheEntry *graphics_find_entry(BacaGraphicsContext *context,
                                                   const BacaDocument *document,
                                                   const char *uri,
                                                   int columns, int rows) {
  for (size_t index = 0U; index < context->entry_count; ++index) {
    if (context->entries[index].document == document &&
        context->entries[index].document_instance_id == document->instance_id &&
        context->entries[index].columns == columns &&
        context->entries[index].rows == rows &&
        strcmp(context->entries[index].uri, uri) == 0) {
      return &context->entries[index];
    }
  }
  return NULL;
}

static BacaGraphicsCacheEntry *graphics_find_image(BacaGraphicsContext *context,
                                                   uint32_t image_id) {
  for (size_t index = 0U; index < context->entry_count; ++index) {
    if (context->entries[index].image_id == image_id) {
      return &context->entries[index];
    }
  }
  return NULL;
}

static bool graphics_remove_entry(BacaGraphicsContext *context, size_t index,
                                  BacaError *error) {
  const size_t removed = context->entries[index].bytes;
  if (!graphics_delete_remote_image(context, context->entries[index].image_id,
                                    context->writer, context->writer_data,
                                    error)) {
    return false;
  }
  graphics_entry_free(&context->entries[index]);
  if (index + 1U < context->entry_count) {
    memmove(&context->entries[index], &context->entries[index + 1U],
            (context->entry_count - index - 1U) * sizeof(*context->entries));
  }
  --context->entry_count;
  context->bytes = removed <= context->bytes ? context->bytes - removed : 0U;
  return true;
}

static bool graphics_make_room(BacaGraphicsContext *context, size_t additional,
                               uint32_t preserve_image, BacaError *error) {
  if (additional > context->maximum_bytes) {
    baca_error_set(error, BACA_ERROR_MEMORY,
                   "target image exceeds graphics cache limit");
    return false;
  }
  while (context->bytes > context->maximum_bytes - additional) {
    size_t oldest = SIZE_MAX;
    for (size_t index = 0U; index < context->entry_count; ++index) {
      if (context->entries[index].image_id == preserve_image) {
        continue;
      }
      if (oldest == SIZE_MAX || context->entries[index].last_used <
                                    context->entries[oldest].last_used) {
        oldest = index;
      }
    }
    if (oldest == SIZE_MAX) {
      baca_error_set(error, BACA_ERROR_MEMORY,
                     "graphics cache cannot evict the retained image");
      return false;
    }
    if (!graphics_remove_entry(context, oldest, error)) {
      return false;
    }
  }
  return true;
}

static uint32_t graphics_image_id(BacaGraphicsContext *context) {
  for (uint32_t attempt = 0U;
       attempt < (uint32_t)BACA_GRAPHICS_CONTEXT_ID_SPAN - 1U; ++attempt) {
    context->image_id_offset =
        context->image_id_offset >=
                (uint32_t)BACA_GRAPHICS_CONTEXT_ID_SPAN - 1U
            ? 1U
            : context->image_id_offset + 1U;
    const uint32_t candidate =
        context->image_id_base + context->image_id_offset;
    if (candidate != 0U && graphics_find_image(context, candidate) == NULL &&
        graphics_remote_image_index(context, candidate) == SIZE_MAX) {
      return candidate;
    }
  }
  return 0U;
}

static GdkPixbuf *graphics_decode_target(const BacaResource *resource,
                                         int width, int height,
                                         BacaError *error) {
  if (resource->length > BACA_GRAPHICS_MAX_INPUT_BYTES) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "compressed image exceeds the supported size limit");
    return NULL;
  }
  if (graphics_resource_has_multiple_frames(resource)) {
    baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                   "animated images are not rendered");
    return NULL;
  }
  GError *gerror = NULL;
  GdkPixbufLoader *loader = graphics_loader_new(resource->mime_type, &gerror);
  if (loader == NULL) {
    baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                   "no GdkPixbuf loader for image: %s",
                   gerror == NULL ? "unknown format" : gerror->message);
    g_clear_error(&gerror);
    return NULL;
  }
  BacaProbeState state = {
      .target_width = width,
      .target_height = height,
  };
  (void)g_signal_connect(loader, "size-prepared",
                         G_CALLBACK(graphics_probe_size), &state);
  const bool wrote =
      gdk_pixbuf_loader_write(loader, resource->data, resource->length,
                              &gerror) != FALSE;
  const bool closed =
      wrote && gdk_pixbuf_loader_close(loader, &gerror) != FALSE;
  const bool animated = closed && graphics_loader_is_animated(loader);
  GdkPixbuf *pixbuf = NULL;
  if (closed && !state.invalid && !animated) {
    pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf != NULL) {
      g_object_ref(pixbuf);
    }
  }
  g_object_unref(loader);
  if (pixbuf == NULL) {
    if (state.invalid) {
      baca_error_set(error, BACA_ERROR_CORRUPT,
                     "image dimensions exceed the supported limits");
    } else if (animated) {
      baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                     "animated images are not rendered");
    } else {
      baca_error_set(error, BACA_ERROR_CORRUPT, "cannot decode image: %s",
                     gerror == NULL ? "invalid image data" : gerror->message);
    }
  }
  g_clear_error(&gerror);
  return pixbuf;
}

static GdkPixbuf *graphics_scale_composited(GdkPixbuf *source, int width,
                                             int height, uint32_t background,
                                             BacaError *error) {
  const int decoded_width = gdk_pixbuf_get_width(source);
  const int decoded_height = gdk_pixbuf_get_height(source);
  if (decoded_width <= 0 || decoded_height <= 0 ||
      decoded_width > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
      decoded_height > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
      (size_t)decoded_width >
          BACA_GRAPHICS_MAX_RENDER_PIXELS / (size_t)decoded_height) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "decoder ignored the bounded image target");
    return NULL;
  }
  GdkPixbuf *scaled = decoded_width == width && decoded_height == height
                          ? g_object_ref(source)
                          : gdk_pixbuf_scale_simple(source, width, height,
                                                    GDK_INTERP_BILINEAR);
  if (scaled == NULL) {
    baca_error_set(error, BACA_ERROR_MEMORY, "cannot scale image");
    return NULL;
  }
  GdkPixbuf *result =
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  if (result == NULL) {
    g_object_unref(scaled);
    baca_error_set(error, BACA_ERROR_MEMORY,
                   "cannot allocate composited image");
    return NULL;
  }

  const int source_channels = gdk_pixbuf_get_n_channels(scaled);
  const int source_rowstride = gdk_pixbuf_get_rowstride(scaled);
  const int result_rowstride = gdk_pixbuf_get_rowstride(result);
  const bool has_alpha =
      gdk_pixbuf_get_has_alpha(scaled) != FALSE && source_channels >= 4;
  const unsigned char *source_pixels = gdk_pixbuf_read_pixels(scaled);
  unsigned char *result_pixels = gdk_pixbuf_get_pixels(result);
  for (int row = 0; row < height; ++row) {
    const unsigned char *source_row =
        source_pixels + (size_t)row * (size_t)source_rowstride;
    unsigned char *result_row =
        result_pixels + (size_t)row * (size_t)result_rowstride;
    for (int column = 0; column < width; ++column) {
      const unsigned char *source_pixel =
          source_row + (size_t)column * (size_t)source_channels;
      unsigned char rgba[4] = {source_pixel[0], source_pixel[1],
                               source_pixel[2], 255U};
      if (has_alpha) {
        rgba[3] = source_pixel[3];
      }
      baca_graphics_composite_pixel(background, rgba,
                                    result_row + (size_t)column * 3U);
    }
  }
  g_object_unref(scaled);
  return result;
}

static bool graphics_estimated_bytes(int width, int height, unsigned channels,
                                     size_t *bytes) {
  if (width <= 0 || height <= 0 || channels == 0U ||
      (size_t)width > SIZE_MAX / channels) {
    return false;
  }
  size_t rowstride = (size_t)width * channels;
  if (rowstride > SIZE_MAX - 3U) {
    return false;
  }
  rowstride = (rowstride + 3U) & ~(size_t)3U;
  if ((size_t)height > SIZE_MAX / rowstride) {
    return false;
  }
  *bytes = rowstride * (size_t)height;
  return true;
}

bool baca_graphics_prepare(BacaGraphicsContext *context, BacaDocument *document,
                           size_t block_index, int columns, int rows,
                           BacaGraphicsSurface *surface, BacaError *error) {
  if (context == NULL || document == NULL || surface == NULL ||
      block_index >= document->block_count || document->blocks == NULL ||
      columns <= 0 || rows <= 0 || surface->pixels != NULL ||
      surface->backing != NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid image preparation request");
    return false;
  }
  *surface = (BacaGraphicsSurface){0};
  if (columns > BACA_GRAPHICS_MAX_COLUMNS) {
    columns = BACA_GRAPHICS_MAX_COLUMNS;
  }
  if (rows > BACA_GRAPHICS_MAX_ROWS) {
    rows = BACA_GRAPHICS_MAX_ROWS;
  }
  BacaBlock *block = &document->blocks[block_index];
  const bool rendered_page = block->kind == BACA_BLOCK_IMAGE &&
                             block->value.image.page_index >= 0 &&
                             document->ops != NULL &&
                             document->ops->render_page != NULL;
  if (block->kind != BACA_BLOCK_IMAGE || block->value.image.uri == NULL ||
      block->value.image.broken || block->value.image.intrinsic_width <= 0 ||
      block->value.image.intrinsic_height <= 0 ||
      (!rendered_page &&
       (block->value.image.intrinsic_width >
            BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
        block->value.image.intrinsic_height >
            BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
        (size_t)block->value.image.intrinsic_width >
            BACA_GRAPHICS_MAX_SOURCE_PIXELS /
                (size_t)block->value.image.intrinsic_height))) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "image block has no decodable resource");
    return false;
  }

  BacaGraphicsCacheEntry *entry = graphics_find_entry(
      context, document, block->value.image.uri, columns, rows);
  if (entry == NULL) {
    int target_width = columns;
    int target_height = rows * 2;
    if (rendered_page) {
      if (columns > INT_MAX / context->cell_pixel_width ||
          rows > INT_MAX / context->cell_pixel_height) {
        baca_error_set(error, BACA_ERROR_MEMORY,
                       "PDF render dimensions overflow");
        return false;
      }
      target_width = columns * context->cell_pixel_width;
      target_height = rows * context->cell_pixel_height;
    }
    size_t estimated = 0U;
    if (target_width > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
        target_height > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
        (size_t)target_width >
            BACA_GRAPHICS_MAX_RENDER_PIXELS / (size_t)target_height ||
        !graphics_estimated_bytes(target_width, target_height, 3U, &estimated) ||
        estimated > context->maximum_bytes) {
      baca_error_set(error, BACA_ERROR_MEMORY,
                     "target image exceeds graphics cache limit");
      return false;
    }
    BacaResource resource = {0};
    const bool loaded = rendered_page
                            ? baca_document_render_page(
                                  document, block->value.image.page_index,
                                  target_width, target_height,
                                  context->background, &resource, error)
                            : baca_document_load_resource(
                                  document, block->value.image.uri, &resource,
                                  error);
    if (!loaded) {
      return false;
    }
    GdkPixbuf *decoded =
        graphics_decode_target(&resource, target_width, target_height, error);
    baca_resource_free(&resource);
    if (decoded == NULL) {
      return false;
    }
    GdkPixbuf *pixbuf = graphics_scale_composited(
        decoded, target_width, target_height, context->background, error);
    g_object_unref(decoded);
    if (pixbuf == NULL) {
      return false;
    }
    const size_t bytes = graphics_pixbuf_bytes(pixbuf);
    if (bytes == SIZE_MAX ||
        !graphics_make_room(context, bytes, 0U, error)) {
      g_object_unref(pixbuf);
      if (!baca_error_is_set(error)) {
        baca_error_set(error, BACA_ERROR_MEMORY,
                       "target image exceeds graphics cache limit");
      }
      return false;
    }
    char *uri = baca_strdup(block->value.image.uri, error);
    if (uri == NULL) {
      g_object_unref(pixbuf);
      return false;
    }
    const uint32_t image_id = graphics_image_id(context);
    if (image_id == 0U) {
      free(uri);
      g_object_unref(pixbuf);
      baca_error_set(error, BACA_ERROR_INTERNAL,
                     "cannot allocate a unique Kitty image id");
      return false;
    }
    BacaError reserve_error = {0};
    BacaGraphicsCacheEntry *entries = baca_array_reserve(
        context->entries, &context->entry_capacity, sizeof(*context->entries),
        context->entry_count + 1U, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
      free(uri);
      g_object_unref(pixbuf);
      if (error != NULL) {
        *error = reserve_error;
      }
      return false;
    }
    context->entries = entries;
    context->entries[context->entry_count++] = (BacaGraphicsCacheEntry){
        .pixbuf = pixbuf,
        .uri = uri,
        .document = document,
        .document_instance_id = document->instance_id,
        .bytes = bytes,
        .last_used = ++context->tick,
        .image_id = image_id,
        .columns = columns,
        .rows = rows,
    };
    context->bytes += bytes;
    entry = graphics_find_entry(context, document, block->value.image.uri,
                                columns, rows);
  }

  if (entry == NULL) {
    baca_error_set(error, BACA_ERROR_INTERNAL,
                   "prepared image disappeared from the cache");
    return false;
  }
  entry->last_used = ++context->tick;
  GdkPixbuf *backing = g_object_ref(entry->pixbuf);
  surface->pixels = gdk_pixbuf_read_pixels(backing);
  surface->pixel_bytes = graphics_pixbuf_bytes(backing);
  surface->width = gdk_pixbuf_get_width(backing);
  surface->height = gdk_pixbuf_get_height(backing);
  surface->columns = entry->columns;
  surface->rows = entry->rows;
  surface->rowstride = gdk_pixbuf_get_rowstride(backing);
  surface->image_id = entry->image_id;
  surface->backing = backing;
  return true;
}

void baca_graphics_surface_release(BacaGraphicsSurface *surface) {
  if (surface == NULL) {
    return;
  }
  if (surface->backing != NULL) {
    g_object_unref(surface->backing);
  }
  *surface = (BacaGraphicsSurface){0};
}

static bool graphics_valid_surface(const BacaGraphicsSurface *surface) {
  const int columns = surface != NULL && surface->columns > 0
                          ? surface->columns
                          : surface == NULL ? 0 : surface->width;
  const int rows = surface != NULL && surface->rows > 0
                       ? surface->rows
                       : surface == NULL ? 0 : (surface->height + 1) / 2;
  return surface != NULL && surface->pixels != NULL && surface->width > 0 &&
          surface->height > 0 &&
          surface->width <= BACA_GRAPHICS_MAX_SOURCE_DIMENSION &&
          surface->height <= BACA_GRAPHICS_MAX_SOURCE_DIMENSION &&
          (size_t)surface->width <=
              BACA_GRAPHICS_MAX_SOURCE_PIXELS / (size_t)surface->height &&
          columns > 0 && columns <= BACA_GRAPHICS_MAX_COLUMNS && rows > 0 &&
          rows <= BACA_GRAPHICS_MAX_ROWS &&
          surface->rowstride >= surface->width * 3 &&
          surface->rowstride <= BACA_GRAPHICS_MAX_SOURCE_DIMENSION * 4 &&
         surface->pixel_bytes >= (size_t)surface->rowstride &&
         (size_t)surface->height <=
             surface->pixel_bytes / (size_t)surface->rowstride;
}

static int graphics_surface_columns(const BacaGraphicsSurface *surface) {
  return surface->columns > 0 ? surface->columns : surface->width;
}

static int graphics_surface_rows(const BacaGraphicsSurface *surface) {
  return surface->rows > 0 ? surface->rows : (surface->height + 1) / 2;
}

static bool
graphics_valid_placement(const BacaGraphicsPlacement *placement) {
  return placement != NULL && placement->viewport_rows >= 0 &&
         placement->viewport_columns >= 0 &&
         placement->occlusion_count <= BACA_GRAPHICS_MAX_OCCLUSIONS &&
         (placement->occlusion_count == 0U ||
          placement->occlusions != NULL);
}

static BacaGraphicsRect
graphics_visible_bounds(const BacaGraphicsSurface *surface,
                          const BacaGraphicsPlacement *placement) {
  const int64_t image_rows = graphics_surface_rows(surface);
  const int64_t image_columns = graphics_surface_columns(surface);
  const int64_t viewport_rows =
      placement->viewport_rows > BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION
          ? BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION
          : placement->viewport_rows;
  const int64_t viewport_columns =
      placement->viewport_columns > BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION
          ? BACA_GRAPHICS_MAX_VIEWPORT_DIMENSION
          : placement->viewport_columns;
  int64_t top = placement->row > 0 ? placement->row : 0;
  int64_t left = placement->column > 0 ? placement->column : 0;
  int64_t bottom = (int64_t)placement->row + image_rows;
  int64_t right = (int64_t)placement->column + image_columns;
  if (bottom > viewport_rows) {
    bottom = viewport_rows;
  }
  if (right > viewport_columns) {
    right = viewport_columns;
  }
  if (top > viewport_rows) {
    top = viewport_rows;
  }
  if (left > viewport_columns) {
    left = viewport_columns;
  }
  return (BacaGraphicsRect){
      .row = (int)top,
      .column = (int)left,
      .rows = bottom > top ? (int)(bottom - top) : 0,
      .columns = right > left ? (int)(right - left) : 0,
  };
}

static bool graphics_rect_contains(const BacaGraphicsRect *rect, int row,
                                    int column) {
  return rect->rows > 0 && rect->columns > 0 && row >= rect->row &&
         column >= rect->column &&
         (int64_t)row < (int64_t)rect->row + rect->rows &&
         (int64_t)column < (int64_t)rect->column + rect->columns;
}

static bool graphics_occluded(const BacaGraphicsPlacement *placement, int row,
                              int column) {
  for (size_t index = 0U; index < placement->occlusion_count; ++index) {
    if (graphics_rect_contains(&placement->occlusions[index], row, column)) {
      return true;
    }
  }
  return false;
}

static uint32_t graphics_surface_pixel(const BacaGraphicsSurface *surface,
                                       int x, int y) {
  if (x < 0) {
    x = 0;
  } else if (x >= surface->width) {
    x = surface->width - 1;
  }
  if (y < 0) {
    y = 0;
  } else if (y >= surface->height) {
    y = surface->height - 1;
  }
  const unsigned char *pixel =
      surface->pixels + (size_t)y * (size_t)surface->rowstride + (size_t)x * 3U;
  return ((uint32_t)pixel[0] << 16U) | ((uint32_t)pixel[1] << 8U) |
         (uint32_t)pixel[2];
}

static int graphics_source_column(const BacaGraphicsSurface *surface,
                                  int display_column) {
  const int columns = graphics_surface_columns(surface);
  if (display_column <= 0) {
    return 0;
  }
  if (display_column >= columns) {
    return surface->width;
  }
  return (int)((int64_t)display_column * surface->width / columns);
}

static int graphics_source_row(const BacaGraphicsSurface *surface,
                               int display_row) {
  const int rows = graphics_surface_rows(surface);
  if (display_row <= 0) {
    return 0;
  }
  if (display_row >= rows) {
    return surface->height;
  }
  return (int)((int64_t)display_row * surface->height / rows);
}

static bool graphics_has_visible_cell(const BacaGraphicsPlacement *placement,
                                      BacaGraphicsRect bounds) {
  for (int row = bounds.row; row < bounds.row + bounds.rows; ++row) {
    for (int column = bounds.column; column < bounds.column + bounds.columns;
         ++column) {
      if (!graphics_occluded(placement, row, column)) {
        return true;
      }
    }
  }
  return false;
}

static bool graphics_write(BacaGraphicsWriter writer, void *user_data,
                           const void *data, size_t length, BacaError *error) {
  if (length == 0U || writer(user_data, data, length)) {
    return true;
  }
  baca_error_set(error, BACA_ERROR_IO,
                 "cannot write terminal graphics escape sequence");
  return false;
}

bool baca_graphics_render_ansi(const BacaGraphicsSurface *surface,
                               const BacaGraphicsPlacement *placement,
                               BacaGraphicsWriter writer, void *user_data,
                               BacaError *error) {
  if (!graphics_valid_surface(surface) ||
      !graphics_valid_placement(placement) || writer == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid ANSI image rendering request");
    return false;
  }
  const BacaGraphicsRect bounds = graphics_visible_bounds(surface, placement);
  if (!graphics_has_visible_cell(placement, bounds)) {
    return true;
  }
  if (!graphics_write(writer, user_data, "\0337", 2U, error)) {
    return false;
  }

  for (int row = bounds.row; row < bounds.row + bounds.rows; ++row) {
    int column = bounds.column;
    while (column < bounds.column + bounds.columns) {
      while (column < bounds.column + bounds.columns &&
             graphics_occluded(placement, row, column)) {
        ++column;
      }
      if (column >= bounds.column + bounds.columns) {
        break;
      }
      char cursor[64] = {0};
      const int cursor_length =
          snprintf(cursor, sizeof(cursor), "\033[%d;%dH", row + 1, column + 1);
      if (cursor_length < 0 || (size_t)cursor_length >= sizeof(cursor) ||
          !graphics_write(writer, user_data, cursor, (size_t)cursor_length,
                          error)) {
        return false;
      }
      while (column < bounds.column + bounds.columns &&
             !graphics_occluded(placement, row, column)) {
        const int source_x =
            graphics_source_column(surface, (int)((int64_t)column -
                                                  (int64_t)placement->column));
        const int display_row =
            (int)((int64_t)row - (int64_t)placement->row);
        const int source_y = graphics_source_row(surface, display_row);
        const int source_bottom =
            graphics_source_row(surface, display_row + 1) - 1;
        const uint32_t foreground =
            graphics_surface_pixel(surface, source_x, source_y);
        const uint32_t background =
            graphics_surface_pixel(surface, source_x, source_bottom);
        char colors[96] = {0};
        const int colors_length =
            snprintf(colors, sizeof(colors),
                     "\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um\xe2\x96\x80",
                     (foreground >> 16U) & 0xffU, (foreground >> 8U) & 0xffU,
                     foreground & 0xffU, (background >> 16U) & 0xffU,
                     (background >> 8U) & 0xffU, background & 0xffU);
        if (colors_length < 0 || (size_t)colors_length >= sizeof(colors) ||
            !graphics_write(writer, user_data, colors, (size_t)colors_length,
                            error)) {
          return false;
        }
        ++column;
      }
    }
  }
  return graphics_write(writer, user_data, "\033[0m\0338", 6U, error);
}

bool baca_graphics_render_cells(const BacaGraphicsSurface *surface,
                                const BacaGraphicsPlacement *placement,
                                BacaGraphicsCellWriter writer, void *user_data,
                                BacaError *error) {
  if (!graphics_valid_surface(surface) ||
      !graphics_valid_placement(placement) || writer == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid image cell rendering request");
    return false;
  }
  const BacaGraphicsRect bounds = graphics_visible_bounds(surface, placement);
  for (int row = bounds.row; row < bounds.row + bounds.rows; ++row) {
    for (int column = bounds.column; column < bounds.column + bounds.columns;
         ++column) {
      if (graphics_occluded(placement, row, column)) {
        continue;
      }
      const int source_x =
          graphics_source_column(surface, (int)((int64_t)column -
                                                (int64_t)placement->column));
      const int display_row =
          (int)((int64_t)row - (int64_t)placement->row);
      const int source_y = graphics_source_row(surface, display_row);
      const int source_bottom =
          graphics_source_row(surface, display_row + 1) - 1;
      if (!writer(user_data, row, column,
                   graphics_surface_pixel(surface, source_x, source_y),
                   graphics_surface_pixel(surface, source_x, source_bottom))) {
        baca_error_set(error, BACA_ERROR_EXTERNAL,
                       "cannot draw terminal image cell");
        return false;
      }
    }
  }
  return true;
}

static uint32_t graphics_palette_rgb(unsigned index) {
  static const uint32_t basic[] = {
      0x000000U, 0x800000U, 0x008000U, 0x808000U, 0x000080U, 0x800080U,
      0x008080U, 0xc0c0c0U, 0x808080U, 0xff0000U, 0x00ff00U, 0xffff00U,
      0x0000ffU, 0xff00ffU, 0x00ffffU, 0xffffffU,
  };
  if (index < BACA_ARRAY_LEN(basic)) {
    return basic[index];
  }
  if (index < 232U) {
    const unsigned cube = index - 16U;
    const unsigned red = cube / 36U;
    const unsigned green = (cube / 6U) % 6U;
    const unsigned blue = cube % 6U;
    const unsigned levels[] = {0U, 95U, 135U, 175U, 215U, 255U};
    return (levels[red] << 16U) | (levels[green] << 8U) | levels[blue];
  }
  const unsigned gray = 8U + (index > 255U ? 23U : index - 232U) * 10U;
  return (gray << 16U) | (gray << 8U) | gray;
}

static uint32_t graphics_palette_88_rgb(unsigned index) {
  if (index < 16U) {
    return graphics_palette_rgb(index);
  }
  if (index < 80U) {
    static const unsigned levels[] = {0U, 139U, 205U, 255U};
    const unsigned cube = index - 16U;
    const unsigned red = cube / 16U;
    const unsigned green = (cube / 4U) % 4U;
    const unsigned blue = cube % 4U;
    return (levels[red] << 16U) | (levels[green] << 8U) | levels[blue];
  }
  static const unsigned grays[] = {46U,  92U,  115U, 139U,
                                   162U, 185U, 208U, 231U};
  const unsigned gray = grays[index < 88U ? index - 80U : 7U];
  return (gray << 16U) | (gray << 8U) | gray;
}

static uint64_t graphics_color_distance(uint32_t left, uint32_t right) {
  const int red = (int)((left >> 16U) & 0xffU) - (int)((right >> 16U) & 0xffU);
  const int green = (int)((left >> 8U) & 0xffU) - (int)((right >> 8U) & 0xffU);
  const int blue = (int)(left & 0xffU) - (int)(right & 0xffU);
  return (uint64_t)(red * red) + (uint64_t)(green * green) +
         (uint64_t)(blue * blue);
}

unsigned baca_graphics_rgb_to_256(uint32_t rgb) {
  return baca_graphics_rgb_to_palette(rgb, 256U);
}

unsigned baca_graphics_rgb_to_palette(uint32_t rgb, unsigned colors) {
  unsigned limit = colors;
  bool palette_88 = false;
  if (colors >= 256U) {
    limit = 256U;
  } else if (colors >= 88U) {
    limit = 88U;
    palette_88 = true;
  } else if (colors >= 16U) {
    limit = 16U;
  } else if (colors >= 8U) {
    limit = 8U;
  } else if (limit == 0U) {
    return 0U;
  }
  unsigned best = 0U;
  uint64_t best_distance = UINT64_MAX;
  for (unsigned index = 0U; index < limit; ++index) {
    const uint32_t candidate = palette_88 ? graphics_palette_88_rgb(index)
                                         : graphics_palette_rgb(index);
    const uint64_t distance = graphics_color_distance(rgb, candidate);
    if (distance < best_distance) {
      best = index;
      best_distance = distance;
    }
  }
  return best;
}

short baca_graphics_pair(BacaGraphicsContext *context, unsigned foreground,
                         unsigned background, short first_pair,
                         short pair_capacity, bool *created) {
  if (created != NULL) {
    *created = false;
  }
  if (context == NULL || first_pair <= 0 || pair_capacity <= 0 ||
      foreground > 255U || background > 255U) {
    return 0;
  }
  for (size_t index = 0U; index < context->pair_count; ++index) {
    if (context->pairs[index].foreground == foreground &&
        context->pairs[index].background == background) {
      return context->pairs[index].pair;
    }
  }
  size_t limit = (size_t)pair_capacity;
  if (limit > BACA_GRAPHICS_PAIR_CACHE_LIMIT) {
    limit = BACA_GRAPHICS_PAIR_CACHE_LIMIT;
  }
  if (context->pair_count < limit &&
      (int)first_pair + (int)context->pair_count <= SHRT_MAX) {
    const short pair = (short)((int)first_pair + (int)context->pair_count);
    context->pairs[context->pair_count++] = (BacaGraphicsPairEntry){
        .foreground = foreground,
        .background = background,
        .pair = pair,
    };
    if (created != NULL) {
      *created = true;
    }
    return pair;
  }

  size_t nearest = 0U;
  uint64_t nearest_distance = UINT64_MAX;
  for (size_t index = 0U; index < context->pair_count; ++index) {
    const uint64_t distance =
        graphics_color_distance(
            graphics_palette_rgb(foreground),
            graphics_palette_rgb(context->pairs[index].foreground)) +
        graphics_color_distance(
            graphics_palette_rgb(background),
            graphics_palette_rgb(context->pairs[index].background));
    if (distance < nearest_distance) {
      nearest = index;
      nearest_distance = distance;
    }
  }
  return context->pair_count == 0U ? 0 : context->pairs[nearest].pair;
}

size_t baca_graphics_pair_count(const BacaGraphicsContext *context) {
  return context == NULL ? 0U : context->pair_count;
}

static bool graphics_write_kitty(BacaGraphicsContext *context,
                                 BacaGraphicsWriter writer, void *user_data,
                                 const char *command, size_t length,
                                 BacaError *error) {
  if (context->multiplexer == BACA_GRAPHICS_MULTIPLEXER_NONE) {
    return graphics_write(writer, user_data, command, length, error);
  }
  BacaString wrapped = {0};
  const char *prefix = context->multiplexer == BACA_GRAPHICS_MULTIPLEXER_TMUX
                           ? "\033Ptmux;"
                           : "\033P";
  if (!baca_string_append(&wrapped, prefix, error)) {
    return false;
  }
  for (size_t index = 0U; index < length; ++index) {
    if (context->multiplexer == BACA_GRAPHICS_MULTIPLEXER_TMUX &&
        command[index] == '\033' &&
        !baca_string_append_char(&wrapped, '\033', error)) {
      baca_string_free(&wrapped);
      return false;
    }
    if (!baca_string_append_char(&wrapped, command[index], error)) {
      baca_string_free(&wrapped);
      return false;
    }
  }
  const bool built = baca_string_append(&wrapped, "\033\\", error);
  if (built && context->multiplexer == BACA_GRAPHICS_MULTIPLEXER_SCREEN &&
      wrapped.length >= BACA_GRAPHICS_KITTY_SCREEN_SEQUENCE_BYTES) {
    baca_string_free(&wrapped);
    baca_error_set(error, BACA_ERROR_INTERNAL,
                   "GNU Screen Kitty sequence reaches the 768-byte limit");
    return false;
  }
  const bool written = built && graphics_write(writer, user_data, wrapped.data,
                                                wrapped.length, error);
  baca_string_free(&wrapped);
  return written;
}

static char *graphics_base64(const unsigned char *data, size_t length,
                             size_t *output_length, BacaError *error) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (length > (SIZE_MAX - 2U) / 3U ||
      ((length + 2U) / 3U) > (SIZE_MAX - 1U) / 4U) {
    baca_error_set(error, BACA_ERROR_MEMORY, "base64 image chunk is too large");
    return NULL;
  }
  const size_t encoded_length = ((length + 2U) / 3U) * 4U;
  char *encoded = malloc(encoded_length + 1U);
  if (encoded == NULL) {
    baca_error_set(error, BACA_ERROR_MEMORY,
                   "cannot allocate base64 image chunk");
    return NULL;
  }
  size_t input = 0U;
  size_t output = 0U;
  while (input < length) {
    const size_t remaining = length - input;
    const unsigned first = data[input++];
    const unsigned second = remaining > 1U ? data[input++] : 0U;
    const unsigned third = remaining > 2U ? data[input++] : 0U;
    encoded[output++] = alphabet[first >> 2U];
    encoded[output++] = alphabet[((first & 0x03U) << 4U) | (second >> 4U)];
    encoded[output++] = remaining > 1U
                            ? alphabet[((second & 0x0fU) << 2U) | (third >> 6U)]
                            : '=';
    encoded[output++] = remaining > 2U ? alphabet[third & 0x3fU] : '=';
  }
  encoded[output] = '\0';
  *output_length = output;
  return encoded;
}

static size_t graphics_remote_image_index(const BacaGraphicsContext *context,
                                          uint32_t image_id) {
  for (size_t index = 0U; index < context->remote_image_count; ++index) {
    if (context->remote_images[index] == image_id) {
      return index;
    }
  }
  return SIZE_MAX;
}

static bool graphics_remote_add_image(BacaGraphicsContext *context,
                                      uint32_t image_id, BacaError *error) {
  if (graphics_remote_image_index(context, image_id) != SIZE_MAX) {
    return true;
  }
  if (context->remote_image_count >= BACA_GRAPHICS_MAX_REMOTE_IMAGES) {
    baca_error_set(error, BACA_ERROR_MEMORY,
                   "too many live Kitty images in this context");
    return false;
  }
  BacaError reserve_error = {0};
  uint32_t *images = baca_array_reserve(
      context->remote_images, &context->remote_image_capacity,
      sizeof(*context->remote_images), context->remote_image_count + 1U,
      &reserve_error);
  if (baca_error_is_set(&reserve_error)) {
    if (error != NULL) {
      *error = reserve_error;
    }
    return false;
  }
  context->remote_images = images;
  context->remote_images[context->remote_image_count++] = image_id;
  return true;
}

static void graphics_remote_remove_image(BacaGraphicsContext *context,
                                         size_t index) {
  if (index + 1U < context->remote_image_count) {
    memmove(&context->remote_images[index], &context->remote_images[index + 1U],
            (context->remote_image_count - index - 1U) *
                sizeof(*context->remote_images));
  }
  --context->remote_image_count;
}

static bool graphics_placement_id_in_use(const BacaGraphicsContext *context,
                                         uint32_t placement_id) {
  for (size_t index = 0U; index < context->remote_placement_count; ++index) {
    if (context->remote_placements[index].placement_id == placement_id) {
      return true;
    }
  }
  return false;
}

static uint32_t graphics_next_placement_id(BacaGraphicsContext *context) {
  for (size_t attempt = 0U;
       attempt <= BACA_GRAPHICS_MAX_REMOTE_PLACEMENTS; ++attempt) {
    ++context->next_placement_id;
    if (context->next_placement_id != 0U &&
        !graphics_placement_id_in_use(context, context->next_placement_id)) {
      return context->next_placement_id;
    }
  }
  return 0U;
}

static bool graphics_remote_add_placement(BacaGraphicsContext *context,
                                          uint32_t image_id,
                                          uint32_t placement_id,
                                          BacaError *error) {
  if (context->remote_placement_count >=
      BACA_GRAPHICS_MAX_REMOTE_PLACEMENTS) {
    baca_error_set(error, BACA_ERROR_MEMORY,
                   "too many live Kitty placements in this context");
    return false;
  }
  BacaError reserve_error = {0};
  BacaGraphicsRemotePlacement *placements = baca_array_reserve(
      context->remote_placements, &context->remote_placement_capacity,
      sizeof(*context->remote_placements),
      context->remote_placement_count + 1U, &reserve_error);
  if (baca_error_is_set(&reserve_error)) {
    if (error != NULL) {
      *error = reserve_error;
    }
    return false;
  }
  context->remote_placements = placements;
  context->remote_placements[context->remote_placement_count++] =
      (BacaGraphicsRemotePlacement){
          .image_id = image_id,
          .placement_id = placement_id,
      };
  return true;
}

static void graphics_remote_remove_placement(BacaGraphicsContext *context,
                                             size_t index) {
  if (index + 1U < context->remote_placement_count) {
    memmove(&context->remote_placements[index],
            &context->remote_placements[index + 1U],
            (context->remote_placement_count - index - 1U) *
                sizeof(*context->remote_placements));
  }
  --context->remote_placement_count;
}

static bool graphics_delete_remote_placement(
    BacaGraphicsContext *context, size_t index, BacaGraphicsWriter writer,
    void *user_data, BacaError *error) {
  if (writer == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "Kitty placement deletion requires a writer");
    return false;
  }
  const BacaGraphicsRemotePlacement placement =
      context->remote_placements[index];
  char command[128] = {0};
  const int length = snprintf(
      command, sizeof(command), "\033_Ga=d,d=i,i=%u,p=%u,q=2;\033\\",
      placement.image_id, placement.placement_id);
  if (length <= 0 || (size_t)length >= sizeof(command) ||
      !graphics_write_kitty(context, writer, user_data, command,
                            (size_t)length, error)) {
    return false;
  }
  graphics_remote_remove_placement(context, index);
  return true;
}

static bool graphics_delete_remote_placements_for(
    BacaGraphicsContext *context, uint32_t image_id,
    BacaGraphicsWriter writer, void *user_data, BacaError *error) {
  size_t index = 0U;
  while (index < context->remote_placement_count) {
    if (context->remote_placements[index].image_id != image_id) {
      ++index;
      continue;
    }
    if (!graphics_delete_remote_placement(context, index, writer, user_data,
                                          error)) {
      return false;
    }
  }
  return true;
}

static bool graphics_delete_remote_image(BacaGraphicsContext *context,
                                         uint32_t image_id,
                                         BacaGraphicsWriter writer,
                                         void *user_data, BacaError *error) {
  if (!graphics_delete_remote_placements_for(context, image_id, writer,
                                              user_data, error)) {
    return false;
  }
  const size_t image_index = graphics_remote_image_index(context, image_id);
  if (image_index != SIZE_MAX) {
    if (writer == NULL) {
      baca_error_set(error, BACA_ERROR_ARGUMENT,
                     "Kitty image deletion requires a writer");
      return false;
    }
    char command[96] = {0};
    const int length = snprintf(command, sizeof(command),
                                "\033_Ga=d,d=I,i=%u,q=2;\033\\", image_id);
    if (length <= 0 || (size_t)length >= sizeof(command) ||
        !graphics_write_kitty(context, writer, user_data, command,
                              (size_t)length, error)) {
      return false;
    }
    graphics_remote_remove_image(context, image_index);
  }
  BacaGraphicsCacheEntry *entry = graphics_find_image(context, image_id);
  if (entry != NULL) {
    entry->transmitted = false;
  }
  return true;
}

static bool graphics_delete_all_remote(BacaGraphicsContext *context,
                                       BacaGraphicsWriter writer,
                                       void *user_data, BacaError *error) {
  while (context->remote_placement_count > 0U) {
    if (!graphics_delete_remote_placement(context, 0U, writer, user_data,
                                          error)) {
      return false;
    }
  }
  while (context->remote_image_count > 0U) {
    const uint32_t image_id = context->remote_images[0];
    if (!graphics_delete_remote_image(context, image_id, writer, user_data,
                                      error)) {
      return false;
    }
  }
  return true;
}

bool baca_graphics_kitty_transmit(BacaGraphicsContext *context,
                                  uint32_t image_id, const unsigned char *png,
                                  size_t png_length, BacaGraphicsWriter writer,
                                  void *user_data, BacaError *error) {
  if (context == NULL || image_id == 0U || png == NULL || png_length == 0U ||
      png_length > BACA_GRAPHICS_MAX_TRANSMIT_BYTES || writer == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid Kitty image transmission request");
    return false;
  }
  context->writer = writer;
  context->writer_data = user_data;
  if (graphics_remote_image_index(context, image_id) != SIZE_MAX &&
      !graphics_delete_remote_image(context, image_id, writer, user_data,
                                    error)) {
    return false;
  }
  if (!graphics_remote_add_image(context, image_id, error)) {
    return false;
  }
  const size_t raw_chunk =
      context->multiplexer == BACA_GRAPHICS_MULTIPLEXER_SCREEN
          ? BACA_GRAPHICS_SCREEN_PNG_RAW_CHUNK
          : BACA_GRAPHICS_PNG_RAW_CHUNK;
  size_t offset = 0U;
  bool first = true;
  while (offset < png_length) {
    const size_t raw_length = png_length - offset > raw_chunk
                                  ? raw_chunk
                                  : png_length - offset;
    const bool more = offset + raw_length < png_length;
    size_t encoded_length = 0U;
    char *encoded =
        graphics_base64(png + offset, raw_length, &encoded_length, error);
    if (encoded == NULL) {
      return false;
    }
    if (encoded_length > BACA_GRAPHICS_KITTY_CHUNK_BYTES) {
      free(encoded);
      baca_error_set(error, BACA_ERROR_INTERNAL,
                     "Kitty base64 chunk exceeds protocol limit");
      return false;
    }
    BacaString command = {0};
    char control[128] = {0};
    const int control_length =
        first
            ? snprintf(control, sizeof(control),
                       "\033_Ga=t,f=100,i=%u,q=2,m=%d;", image_id, more ? 1 : 0)
            : snprintf(control, sizeof(control), "\033_Gm=%d;", more ? 1 : 0);
    const bool built =
        control_length > 0 && (size_t)control_length < sizeof(control) &&
        baca_string_append_n(&command, control, (size_t)control_length,
                             error) &&
        baca_string_append_n(&command, encoded, encoded_length, error) &&
        baca_string_append(&command, "\033\\", error);
    free(encoded);
    if (!built || !graphics_write_kitty(context, writer, user_data,
                                        command.data, command.length, error)) {
      baca_string_free(&command);
      return false;
    }
    baca_string_free(&command);
    offset += raw_length;
    first = false;
  }
  return true;
}

static bool graphics_rect_add(BacaGraphicsRect **rects, size_t *count,
                              size_t *capacity, BacaGraphicsRect rect,
                              BacaError *error) {
  if (rect.rows <= 0 || rect.columns <= 0) {
    return true;
  }
  if (*count >= BACA_GRAPHICS_MAX_VISIBLE_RECTS) {
    baca_error_set(error, BACA_ERROR_CORRUPT,
                   "image occlusions produce too many visible regions");
    return false;
  }
  BacaError reserve_error = {0};
  BacaGraphicsRect *resized = baca_array_reserve(
      *rects, capacity, sizeof(**rects), *count + 1U, &reserve_error);
  if (baca_error_is_set(&reserve_error)) {
    if (error != NULL) {
      *error = reserve_error;
    }
    return false;
  }
  *rects = resized;
  (*rects)[(*count)++] = rect;
  return true;
}

static bool graphics_visible_rects(const BacaGraphicsSurface *surface,
                                   const BacaGraphicsPlacement *placement,
                                   BacaGraphicsRect **rects, size_t *rect_count,
                                   BacaError *error) {
  size_t capacity = 0U;
  BacaGraphicsRect bounds = graphics_visible_bounds(surface, placement);
  if (!graphics_rect_add(rects, rect_count, &capacity, bounds, error)) {
    return false;
  }
  for (size_t occlusion_index = 0U;
       occlusion_index < placement->occlusion_count; ++occlusion_index) {
    BacaGraphicsRect *next = NULL;
    size_t next_count = 0U;
    size_t next_capacity = 0U;
    const BacaGraphicsRect cover = placement->occlusions[occlusion_index];
    for (size_t index = 0U; index < *rect_count; ++index) {
      const BacaGraphicsRect rect = (*rects)[index];
      const int64_t top = rect.row > cover.row ? rect.row : cover.row;
      const int64_t left = rect.column > cover.column ? rect.column : cover.column;
      const int64_t rect_bottom = (int64_t)rect.row + rect.rows;
      const int64_t cover_bottom = (int64_t)cover.row + cover.rows;
      const int64_t bottom =
          rect_bottom < cover_bottom ? rect_bottom : cover_bottom;
      const int64_t rect_right = (int64_t)rect.column + rect.columns;
      const int64_t cover_right = (int64_t)cover.column + cover.columns;
      const int64_t right = rect_right < cover_right ? rect_right : cover_right;
      if (top >= bottom || left >= right) {
        if (!graphics_rect_add(&next, &next_count, &next_capacity, rect,
                               error)) {
          free(next);
          free(*rects);
          *rects = NULL;
          *rect_count = 0U;
          return false;
        }
        continue;
      }
      const BacaGraphicsRect pieces[] = {
          {.row = rect.row,
           .column = rect.column,
           .rows = (int)(top - rect.row),
           .columns = rect.columns},
          {.row = (int)bottom,
           .column = rect.column,
           .rows = (int)(rect_bottom - bottom),
           .columns = rect.columns},
          {.row = (int)top,
           .column = rect.column,
           .rows = (int)(bottom - top),
           .columns = (int)(left - rect.column)},
          {.row = (int)top,
           .column = (int)right,
           .rows = (int)(bottom - top),
           .columns = (int)(rect_right - right)},
      };
      for (size_t piece = 0U; piece < BACA_ARRAY_LEN(pieces); ++piece) {
        if (!graphics_rect_add(&next, &next_count, &next_capacity,
                               pieces[piece], error)) {
          free(next);
          free(*rects);
          *rects = NULL;
          *rect_count = 0U;
          return false;
        }
      }
    }
    free(*rects);
    *rects = next;
    *rect_count = next_count;
    capacity = next_capacity;
  }
  return true;
}

bool baca_graphics_kitty_place(BacaGraphicsContext *context,
                               const BacaGraphicsSurface *surface,
                               const BacaGraphicsPlacement *placement,
                               BacaGraphicsWriter writer, void *user_data,
                               BacaError *error) {
  if (context == NULL || !graphics_valid_surface(surface) ||
      !graphics_valid_placement(placement) || surface->image_id == 0U ||
      writer == NULL ||
      graphics_remote_image_index(context, surface->image_id) == SIZE_MAX) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid Kitty image placement request");
    return false;
  }
  context->writer = writer;
  context->writer_data = user_data;
  BacaGraphicsRect *rects = NULL;
  size_t rect_count = 0U;
  if (!graphics_visible_rects(surface, placement, &rects, &rect_count, error)) {
    return false;
  }
  for (size_t index = 0U; index < rect_count; ++index) {
    const BacaGraphicsRect rect = rects[index];
    const int display_x =
        (int)((int64_t)rect.column - (int64_t)placement->column);
    const int display_y =
        (int)((int64_t)rect.row - (int64_t)placement->row);
    const int source_x = graphics_source_column(surface, display_x);
    const int source_y = graphics_source_row(surface, display_y);
    const int source_right =
        graphics_source_column(surface, display_x + rect.columns);
    const int source_bottom =
        graphics_source_row(surface, display_y + rect.rows);
    const uint32_t placement_id = graphics_next_placement_id(context);
    if (placement_id == 0U ||
        !graphics_remote_add_placement(context, surface->image_id,
                                       placement_id, error)) {
      free(rects);
      return false;
    }
    char cursor[64] = {0};
    const int cursor_length =
        snprintf(cursor, sizeof(cursor), "\0337\033[%d;%dH", rect.row + 1,
                 rect.column + 1);
    char command[256] = {0};
    const int command_length = snprintf(
        command, sizeof(command),
        "\033_Ga=p,i=%u,p=%u,q=2,x=%d,y=%d,w=%d,h=%d,c=%d,r=%d,C=1;\033\\",
        surface->image_id, placement_id, source_x, source_y,
        source_right - source_x, source_bottom - source_y, rect.columns,
        rect.rows);
    if (cursor_length <= 0 || (size_t)cursor_length >= sizeof(cursor) ||
        command_length <= 0 || (size_t)command_length >= sizeof(command) ||
        !graphics_write(writer, user_data, cursor, (size_t)cursor_length,
                        error) ||
        !graphics_write_kitty(context, writer, user_data, command,
                              (size_t)command_length, error) ||
        !graphics_write(writer, user_data, "\0338", 2U, error)) {
      free(rects);
      return false;
    }
  }
  free(rects);
  return true;
}

bool baca_graphics_kitty_draw(BacaGraphicsContext *context,
                              const BacaGraphicsSurface *surface,
                              const BacaGraphicsPlacement *placement,
                              BacaGraphicsWriter writer, void *user_data,
                              BacaError *error) {
  if (context == NULL || !graphics_valid_surface(surface) || writer == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "invalid Kitty image draw request");
    return false;
  }
  BacaGraphicsCacheEntry *entry =
      graphics_find_image(context, surface->image_id);
  if (entry == NULL || surface->backing != entry->pixbuf ||
      surface->pixels != gdk_pixbuf_read_pixels(entry->pixbuf)) {
    baca_error_set(error, BACA_ERROR_ARGUMENT,
                   "Kitty surface is stale for this graphics context");
    return false;
  }
  context->writer = writer;
  context->writer_data = user_data;
  if (!entry->transmitted) {
    GError *gerror = NULL;
    gchar *png = NULL;
    gsize png_length = 0U;
    const bool encoded =
        gdk_pixbuf_save_to_buffer(entry->pixbuf, &png, &png_length, "png",
                                  &gerror, NULL) != FALSE;
    if (!encoded) {
      baca_error_set(error, BACA_ERROR_EXTERNAL, "cannot encode Kitty PNG: %s",
                     gerror == NULL ? "PNG encoder unavailable"
                                    : gerror->message);
      g_clear_error(&gerror);
      g_free(png);
      return false;
    }
    const bool transmitted = baca_graphics_kitty_transmit(
        context, surface->image_id, (const unsigned char *)png,
        (size_t)png_length, writer, user_data, error);
    g_free(png);
    g_clear_error(&gerror);
    if (!transmitted) {
      return false;
    }
    entry->transmitted = true;
  }
  return baca_graphics_kitty_place(context, surface, placement, writer,
                                   user_data, error);
}

bool baca_graphics_kitty_delete_placements(BacaGraphicsContext *context,
                                            BacaGraphicsWriter writer,
                                            void *user_data, BacaError *error) {
  if (context == NULL || writer == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid Kitty delete request");
    return false;
  }
  context->writer = writer;
  context->writer_data = user_data;
  while (context->remote_placement_count > 0U) {
    if (!graphics_delete_remote_placement(context, 0U, writer, user_data,
                                          error)) {
      return false;
    }
  }
  return true;
}

bool baca_graphics_kitty_delete_all(BacaGraphicsContext *context,
                                     BacaGraphicsWriter writer, void *user_data,
                                     BacaError *error) {
  if (context == NULL || writer == NULL) {
    baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid Kitty delete request");
    return false;
  }
  context->writer = writer;
  context->writer_data = user_data;
  return graphics_delete_all_remote(context, writer, user_data, error);
}
