#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "DictionaryWordSelectActivity.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  // This stacked reader surface must not run the underlying book's
  // load/persistence path.
  Activity::onEnter();
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition);
    wrapText();
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() { Activity::onExit(); }

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscape = isLandscapeCw || orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;
  const int bodyY = contentY + metrics.topPadding + metrics.headerHeight;
  const int topArea = bodyY;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;
  return {contentX + SIDE_PADDING, bodyY, renderer.getScreenWidth() - hintGutterWidth - 2 * SIDE_PADDING,
          renderer.getScreenHeight() - topArea - bottomArea};
}

bool DictionaryDefinitionActivity::performReaderAction(const ReaderAction action, const int x, const int y) {
  if (action != ReaderAction::LookupAtPoint) return false;
  return openLookupAt(x, y);
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

bool DictionaryDefinitionActivity::findWordAt(const int x, const int y, const char*& word,
                                              uint32_t& terminatorOffset) const {
  constexpr int TOUCH_SLOP = 4;
  word = nullptr;
  terminatorOffset = UINT32_MAX;

  const BodyArea body = bodyArea();
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId);

  if (!pages.empty()) {
    const int ascender = renderer.getFontAscenderSize(fontId);
    for (const auto& element : pages[currentPage]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* line = static_cast<const PageLine*>(element.get());
      const auto& block = line->getBlock();
      if (!block || !block->valid()) continue;

      const int wordY = body.y + line->yPos + block->getRubyShift(ascender);
      if (y < wordY - TOUCH_SLOP || y >= wordY + lineHeight + TOUCH_SLOP) continue;
      for (uint16_t i = 0; i < block->wordCount(); i++) {
        const int wordX = body.x + line->xPos + block->wordXpos(i);
        const int wordWidth = block->renderedWordAdvance(renderer, fontId, i);
        if (x >= wordX - TOUCH_SLOP && x < wordX + wordWidth + TOUCH_SLOP) {
          word = block->wordText(i);
          return true;
        }
      }
    }
    return false;
  }

  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int lineIndex = firstLine; lineIndex < lastLine; lineIndex++) {
    const int lineY = body.y + (lineIndex - firstLine) * lineHeight;
    if (y < lineY - TOUCH_SLOP || y >= lineY + lineHeight + TOUCH_SLOP) continue;

    const Line& line = lines[lineIndex];
    const uint32_t lineEnd = line.start + line.len;
    uint32_t cursor = line.start;
    while (cursor < lineEnd) {
      while (cursor < lineEnd && (definition[cursor] == ' ' || definition[cursor] == '\t' ||
                                  definition[cursor] == '\r' || definition[cursor] == '\n')) {
        cursor++;
      }
      const uint32_t tokenStart = cursor;
      while (cursor < lineEnd && definition[cursor] != ' ' && definition[cursor] != '\t' &&
             definition[cursor] != '\r' && definition[cursor] != '\n') {
        cursor++;
      }
      if (tokenStart == cursor) continue;

      const int wordX = body.x + measureSpan(fontId, definition.c_str() + line.start, tokenStart - line.start);
      const int wordWidth = measureSpan(fontId, definition.c_str() + tokenStart, cursor - tokenStart);
      if (x >= wordX - TOUCH_SLOP && x < wordX + wordWidth + TOUCH_SLOP) {
        word = definition.c_str() + tokenStart;
        terminatorOffset = cursor;
        return true;
      }
    }
    return false;
  }
  return false;
}

bool DictionaryDefinitionActivity::openLookupAt(const int x, const int y) {
  const char* word = nullptr;
  uint32_t terminatorOffset = UINT32_MAX;
  if (!findWordAt(x, y, word, terminatorOffset)) return false;

  char savedTerminator = '\0';
  if (terminatorOffset < definition.size()) {
    savedTerminator = definition[terminatorOffset];
    definition[terminatorOffset] = '\0';
  }

  auto lookupActivity = makeUniqueNoThrow<DictionaryWordSelectActivity>(renderer, mappedInput, word);
  if (!lookupActivity) {
    if (terminatorOffset < definition.size()) definition[terminatorOffset] = savedTerminator;
    LOG_ERR("DICT", "OOM: nested dictionary lookup activity");
    return true;
  }

  startActivityForResult(std::move(lookupActivity), [this, terminatorOffset, savedTerminator](const ActivityResult&) {
    if (terminatorOffset < definition.size()) definition[terminatorOffset] = savedTerminator;
  });
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
}

bool DictionaryDefinitionActivity::pageTurn(const bool isForward) {
  return skipPages(isForward ? 1 : -1);
}

bool DictionaryDefinitionActivity::skipPages(const int amount) {
  const int target = std::clamp(currentPage + amount, 0, totalPages - 1);
  if (target == currentPage) return false;
  currentPage = target;
  return true;
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf);
  }
}

void DictionaryDefinitionActivity::renderBook() {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  // Header: matched headword left, page counter right.
  const int headerY = contentY + metrics.topPadding + 10;
  renderer.drawText(UI_12_FONT_ID, contentX + SIDE_PADDING, headerY, headword.c_str(), true, EpdFontFamily::BOLD);
  if (totalPages > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - SIDE_PADDING - counterWidth, headerY, counter);
  }

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const BodyArea body = bodyArea();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, body.x, body.y);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, body.x, body.y);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", (currentPage > 0 ? "<" : ""), (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}
