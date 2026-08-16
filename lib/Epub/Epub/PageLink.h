#pragma once

#include <cstddef>
#include <cstdint>

struct LinkHitRect {
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
};

struct PageLinkHitRegions {
  static constexpr uint8_t MAX_RECTS = 8;

  uint8_t linkIndex;
  uint8_t rectCount;
  LinkHitRect rects[MAX_RECTS];
};

struct PageLink {
  static constexpr size_t TEXT_CAPACITY = 32;
  static constexpr size_t HREF_CAPACITY = 256;
  static constexpr uint8_t MAX_PER_PAGE = 16;

  // The short text shown for this link. HREF_CAPACITY accommodates long,
  // URL-encoded Calibre-generated filenames.
  char text[TEXT_CAPACITY];
  char href[HREF_CAPACITY];
  uint16_t id;

  PageLink() : text{}, href{}, id(0) {}
};
