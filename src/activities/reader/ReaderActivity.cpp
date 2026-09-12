#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalTiltSensor.h>
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "TxtReaderActivity.h"
#include "XtcReaderActivity.h"

ReaderActivity::ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::string bookPath, const bool allowFastInitialRefresh,
                               const bool returnToPreviousOnBack)
    : Activity(name, renderer, mappedInput),
      bookPath(std::move(bookPath)),
      returnToPreviousOnBack(returnToPreviousOnBack) {
  if (allowFastInitialRefresh) {
    const int refreshFrequency = SETTINGS.getRefreshFrequency();
    pagesUntilFullRefresh = refreshFrequency > 1 ? refreshFrequency : 2;
  }
}

std::unique_ptr<ReaderActivity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string path, const bool allowFastInitialRefresh) {
  // ActivityManager requires heap ownership; each branch allocates exactly one screen-lifetime object.
  std::unique_ptr<ReaderActivity> activity;
  if (FsHelpers::hasXtcExtension(path)) {
    activity = makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    activity = makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else {
    activity = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  if (!activity) {
    LOG_ERR("READER", "OOM: reader activity");
  }
  return activity;
}

void ReaderActivity::applyInitialOrientation() { ReaderUtils::applyOrientation(renderer, SETTINGS.orientation); }

void ReaderActivity::disableFastInitialRefresh() { pagesUntilFullRefresh = 0; }

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", bookPath.c_str());
    finish();
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  applyInitialOrientation();

  if (!loadBook()) {
    finish();
    return;
  }

  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
  requestUpdate();
}

void ReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
}

bool ReaderActivity::handleBackNavigation(const bool backReleased, const unsigned long heldMs) {
  if (!backReleased) return false;
  const bool longPress = heldMs >= ReaderUtils::GO_BACK_OR_HOME_MS;
  if (returnToPreviousOnBack) {
    finish();
  } else if (!longPress && navigateBackWithinContent()) {
    return true;
  } else if (longPress != SETTINGS.backShortToFileBrowser) {
    activityManager.goToFileBrowser(bookPath.c_str());
  } else {
    onGoHome();
  }
  return true;
}

bool ReaderActivity::handleHomeGesture() {
  if (SETTINGS.shortHomePress != CrossPointSettings::SHORT_HOME_PRESS::HOME_BACK) return false;
  if (returnToPreviousOnBack) {
    finish();
    return true;
  }
  return navigateBackWithinContent();
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;

  RenderLock lock(*this);
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::handleEndOfBookMenu(const bool suppressConfirmRelease) {
  if (!isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire) || !endOfBookOptions->menuActive() ||
      suppressConfirmRelease) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }

  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;

  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
    return true;
  }
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

void ReaderActivity::loop() {
  if (updateReaderState()) return;
  clearEndOfBookOptionsIfNeeded();

  const bool touchEnabled = SETTINGS.touchReaderControls && mappedInput.hasTouch();
  int touchX = 0;
  int touchY = 0;
  if (touchEnabled && mappedInput.wasScreenLongPress(touchX, touchY) &&
      performReaderAction(ReaderAction::LookupAtPoint, touchX, touchY)) {
    return;
  }

  const bool tapped = touchEnabled && mappedInput.wasScreenTapped(touchX, touchY);
  if (tapped && performReaderAction(ReaderAction::ActivateAtPoint, touchX, touchY)) return;

  const auto swipe = touchEnabled && SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_SWIPE
                         ? mappedInput.wasSwipe()
                         : MappedInputManager::SwipeDir::None;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int zoneWidth = screenWidth / 3;
  const int zoneHeight = screenHeight / 3;
  const bool centerTap = tapped && SETTINGS.tapForReaderMenu && touchX >= zoneWidth &&
                         touchX < screenWidth - zoneWidth && touchY >= zoneHeight &&
                         touchY < screenHeight - zoneHeight;
  const bool touchMenu = touchEnabled && (mappedInput.wasMenuGesture() || centerTap);
  const bool backGesture = mappedInput.wasBackGesture();
  const bool backReleased = !backGesture && mappedInput.wasReleased(MappedInputManager::Button::Back);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool powerReleased = mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const unsigned long heldMs = mappedInput.getHeldTime();

  if (automaticPageTurnActive) {
    if (confirmReleased || backReleased || touchMenu) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }
    if (!canTurnReaderPage()) {
      requestUpdate();
      return;
    }
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }
    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      requestUpdate();
      return;
    }
  }

  if (confirmReleased) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (heldMs >= ReaderUtils::BOOKMARK_HOLD_MS && performReaderAction(ReaderAction::ToggleBookmark)) return;
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (heldMs >= ReaderUtils::GO_HOME_MS && performReaderAction(ReaderAction::Sync)) return;
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (heldMs >= ReaderUtils::BOOKMARK_HOLD_MS && performReaderAction(ReaderAction::OpenDictionary)) return;
        break;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  if (mappedInput.wasHomeKeyHold()) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (performReaderAction(ReaderAction::ToggleBookmark)) return;
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (performReaderAction(ReaderAction::Sync)) return;
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (performReaderAction(ReaderAction::OpenDictionary)) return;
        break;
      case CrossPointSettings::LP_MENU_READER_MENU:
        if (performReaderAction(ReaderAction::OpenMenu)) return;
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  if (handleEndOfBookMenu()) return;
  if ((confirmReleased || touchMenu) && performReaderAction(ReaderAction::OpenMenu)) return;
  if (handleBackNavigation(backReleased, heldMs)) return;

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::LINKS && powerReleased && !downReleased &&
      performReaderAction(ReaderAction::OpenLinks)) {
    return;
  }

  constexpr unsigned long MIN_MANUAL_TURN_GAP_MS = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < MIN_MANUAL_TURN_GAP_MS;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!canTurnReaderPage()) {
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    pageTurn(forward);
    requestUpdate();
    return;
  }

  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront = mappedInput.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  bool prevTriggered = tiltPrev ||
                       (usePress ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                    mappedInput.wasPressed(prevButton))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                    mappedInput.wasReleased(prevButton)));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased;
  bool nextTriggered = tiltNext ||
                       (usePress ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerTurn ||
                                    mappedInput.wasPressed(nextButton))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerTurn ||
                                    mappedInput.wasReleased(nextButton)));
  const bool fromTilt = tiltPrev || tiltNext;
  unsigned long pageHeldMs = heldMs;

  if (swipe == MappedInputManager::SwipeDir::Left) {
    nextTriggered = true;
    pageHeldMs = 0;
  } else if (swipe == MappedInputManager::SwipeDir::Right) {
    prevTriggered = true;
    pageHeldMs = 0;
  } else if (tapped && SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_SWIPE) {
    const bool inverted = SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
    if (touchX < zoneWidth) {
      prevTriggered = !inverted;
      nextTriggered = inverted;
    } else if (touchX >= screenWidth - zoneWidth) {
      prevTriggered = inverted;
      nextTriggered = !inverted;
    }
  }
  if (!prevTriggered && !nextTriggered) return;
  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;

  if (powerReleased && downReleased) return;

  const bool longPress = !fromTilt && pageHeldMs > ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    skipPages((nextTriggered ? 1 : -1) * longPressSkipAmount());
    requestUpdate();
    return;
  }
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    if (applyReaderOrientation(newOrientation)) {
      requestUpdate();
      return;
    }
  }
  if (!canTurnReaderPage()) {
    requestUpdate();
    return;
  }
  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
  requestUpdate();
}

void ReaderActivity::render(RenderLock&&) {
  if (isAtEndOfBook()) {
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(bookPath);
      // Release-publish AFTER loadOnce() so the main task's acquire load can't
      // observe an object whose names/selector are still being populated.
      endOfBookOptionsReady.store(true, std::memory_order_release);
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    onEndOfBookRendered();
    return;
  }

  renderBook();
}

bool ReaderActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh = 1;
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}
