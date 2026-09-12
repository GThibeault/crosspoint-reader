#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "EndOfBookOptions.h"
#include "activities/Activity.h"

class ReaderActivity : public Activity {
 protected:
  std::string bookPath;
  int pagesUntilFullRefresh = 0;
  bool forcedRefreshPending = false;
  bool returnToPreviousOnBack = false;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool automaticPageTurnActive = false;

  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};

  explicit ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                          std::string bookPath, bool allowFastInitialRefresh, bool returnToPreviousOnBack = false);

  virtual bool loadBook() = 0;
  virtual std::string getBookTitle() const = 0;
  virtual std::string getBookAuthor() const { return ""; }
  virtual std::string getBookThumbBmpPath() const { return ""; }

  virtual bool pageTurn(bool isForward) = 0;
  virtual bool skipPages(int amount) { return pageTurn(amount > 0); }
  virtual bool isAtEndOfBook() const = 0;
  virtual void onReturnFromEndOfBook() {}

  virtual void renderBook() = 0;
  virtual void applyInitialOrientation();
  virtual void onEndOfBookRendered() {}
  virtual bool updateReaderState() { return false; }

  enum class ReaderAction {
    ActivateAtPoint, LookupAtPoint, ToggleBookmark, Sync, OpenDictionary, OpenMenu, OpenLinks
  };
  virtual bool performReaderAction(ReaderAction action, int x = -1, int y = -1) {
    return false;
  }
  virtual bool navigateBackWithinContent() { return false; }
  virtual bool canTurnReaderPage() const { return true; }
  virtual int longPressSkipAmount() const { return 10; }
  virtual bool applyReaderOrientation(uint8_t orientation) { return false; }

  bool handleBackNavigation(bool backTriggered, unsigned long heldMs);
  /** True while the end-of-book suggestion menu is on screen and owning input. */
  bool endOfBookMenuActive() const;
  bool handleEndOfBookMenu(bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool prevTriggered, bool nextTriggered);
  void clearEndOfBookOptionsIfNeeded();
  void disableFastInitialRefresh();

 public:
  ~ReaderActivity() override = default;

  static std::unique_ptr<ReaderActivity> create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                std::string path, bool allowFastInitialRefresh);

  void onEnter() override;
  void onExit() override;
  void loop() final;
  void render(RenderLock&& lock) override;
  bool handleHomeGesture() final;

  bool isReaderActivity() const final { return true; }
  bool appliesNightMode() const final { return true; }
  bool handleForcedRefresh() final;
};
