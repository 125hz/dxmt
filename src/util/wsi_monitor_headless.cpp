/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 *
 * iOS-Madeira note 2026-05-13: Was all-nullptr stub which caused
 * IDXGIAdapter::EnumOutputs to return DXGI_ERROR_NOT_FOUND. Thumper
 * then stored NULL in a global IDXGIOutput slot and crashed calling
 * vtable[7] (GetDesc) through NULL. Implements a synthetic single
 * 1024x768 @ 60Hz monitor so the DXGI output chain succeeds.
 */

#include "wsi_monitor.hpp"

#include "util_env.hpp"
#include "util_madeira_switch.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt::wsi {

/* Synthetic singleton monitor handle. Non-NULL so EnumOutputs sees a
 * monitor exists; opaque value (1) since headless code paths never
 * dereference HMONITOR. */
static HMONITOR const kSyntheticMonitor = reinterpret_cast<HMONITOR>(1);

/* iOS-Madeira 2026-07-07 (task #24): the hardcoded 1024x768 predates the
 * 960x540 virtual desktop — user32 (GetSystemMetrics/GetMonitorInfo) and
 * DXGI disagreed on the screen size, games booted at a mode that crops on
 * the desktop, and Thumper's video-settings page fataled (ExitProcess -1)
 * failing to reconcile the two. Ask user32 for the real screen size; the
 * Wine iOS win32u virtual monitor serves it in every regime (desktop and
 * game mode). Fall back to 1024x768 only if the call fails. */
static void getScreenSize(uint32_t *w, uint32_t *h) {
  int sw = ::GetSystemMetrics(SM_CXSCREEN);
  int sh = ::GetSystemMetrics(SM_CYSCREEN);
  *w = (sw > 0) ? (uint32_t)sw : 1024;
  *h = (sh > 0) ? (uint32_t)sh : 768;
}

/* MADEIRA (ml1190): DXMT_WSI_MONITOR_IDENTITY, opt-in. With it set,
 * DXGI_OUTPUT_DESC::Monitor carries user32's primary monitor handle instead of
 * the private sentinel, so an application can match the output against
 * user32. Every HMONITOR this backend hands out or accepts goes through this
 * function -- including wsi::getWindowMonitor() in wsi_window_headless.cpp,
 * which the swapchain's fullscreen transition compares against
 * DXGI_OUTPUT_DESC::Monitor -- so both sides always agree. Unset, this is the
 * upstream sentinel exactly. */
HMONITOR getDefaultMonitor() {
  static const bool useIdentity = madeiraSwitch("DXMT_WSI_MONITOR_IDENTITY");
  if (useIdentity) {
    HMONITOR monitor = ::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (monitor) {
      static const bool announced = [monitor] {
        Logger::info(str::format("[monitor-identity] ml1190 using user32 primary=", monitor,
            " (DXMT_WSI_MONITOR_IDENTITY)"));
        return true;
      }();
      (void)announced;
      return monitor;
    }
  }
  return kSyntheticMonitor;
}

HMONITOR enumMonitors(uint32_t index) {
  /* Only one synthetic monitor exists. */
  if (index != 0)
    return nullptr;
  return getDefaultMonitor();
}

bool getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) {
  if (hMonitor != getDefaultMonitor())
    return false;
  /* Standard Windows display device name "\\.\DISPLAY1". */
  static const WCHAR kName[] = {'\\','\\','.','\\','D','I','S','P','L','A','Y','1', 0};
  for (size_t i = 0; i < 32; i++)
    Name[i] = (i < sizeof(kName)/sizeof(WCHAR)) ? kName[i] : (WCHAR)0;
  return true;
}

bool getDesktopCoordinates(HMONITOR hMonitor, RECT *pRect) {
  if (hMonitor != getDefaultMonitor() || !pRect)
    return false;
  /* Real screen size from user32 — MUST agree with what win32u's virtual
   * monitor reports (sysparams_ios.c now serves the same values through
   * NtUserEnumDisplaySettings), or games fatal reconciling the two. */
  uint32_t w, h;
  getScreenSize(&w, &h);
  pRect->left = 0;
  pRect->top = 0;
  pRect->right = (LONG)w;
  pRect->bottom = (LONG)h;
  return true;
}

static inline void fillMode(WsiMode *pMode, uint32_t w, uint32_t h) {
  pMode->width = w;
  pMode->height = h;
  pMode->refreshRate.numerator = 60;
  pMode->refreshRate.denominator = 1;
  pMode->bitsPerPixel = 32;
  pMode->interlaced = false;
}

/* Mode list: classic small modes + the real desktop resolution (deduped),
 * all @ 60Hz 32bpp. Mirrors the win32u NtUserEnumDisplaySettings synth so
 * user32 and DXGI tell games the same story. Nothing larger than the
 * desktop — bigger modes crop on the virtual desktop surface. */
static bool getSyntheticDisplayMode(uint32_t modeNumber, WsiMode *pMode) {
  uint32_t sw, sh;
  getScreenSize(&sw, &sh);
  uint32_t widths[3]  = {640, 800, sw};
  uint32_t heights[3] = {480, 600, sh};
  uint32_t count = ((sw == 640 && sh == 480) || (sw == 800 && sh == 600)) ? 2 : 3;
  if (modeNumber >= count)
    return false;
  fillMode(pMode, widths[modeNumber], heights[modeNumber]);
  return true;
}

/* MADEIRA (2026-09-16): DXMT_WSI_MODE_TABLE, opt-in. The list above is
 * "640x480, 800x600 and whatever you are already running" -- three entries
 * at most -- while the iOS win32u virtual monitor offers a full mode table
 * through EnumDisplaySettings, so user32 listed 14+ modes and DXGI listed 3.
 * An application that saved 1024x768, or that walks the adapter modes
 * looking for the one it wants, found nothing. With the switch set the list
 * is EnumDisplaySettingsExW's, which IS the win32u table, verbatim -- no
 * second copy to drift. */
static bool getUser32DisplayMode(uint32_t modeNumber, WsiMode *pMode) {
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  /* NULL device = the primary display. win32u answers every name with the
   * single virtual display, so the name never has to be resolved first. */
  if (!::EnumDisplaySettingsExW(nullptr, (DWORD)modeNumber, &dm, 0))
    return false;
  if (!dm.dmPelsWidth || !dm.dmPelsHeight)
    return false;

  pMode->width = dm.dmPelsWidth;
  pMode->height = dm.dmPelsHeight;
  /* dmDisplayFrequency is 0 or 1 on a driver that does not track a rate;
   * both mean "unspecified", and a 0/1 Hz mode is not something an
   * application can select. */
  pMode->refreshRate.numerator = (dm.dmDisplayFrequency > 1) ? dm.dmDisplayFrequency : 60;
  pMode->refreshRate.denominator = 1;
  pMode->bitsPerPixel = dm.dmBitsPerPel ? dm.dmBitsPerPel : 32;
  pMode->interlaced = (dm.dmDisplayFlags & DM_INTERLACED) != 0;
  return true;
}

bool getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode *pMode) {
  if (hMonitor != getDefaultMonitor() || !pMode)
    return false;
  static const bool useUser32Modes = madeiraSwitch("DXMT_WSI_MODE_TABLE");
  return useUser32Modes ? getUser32DisplayMode(modeNumber, pMode) : getSyntheticDisplayMode(modeNumber, pMode);
}

bool getCurrentDisplayMode(HMONITOR hMonitor, WsiMode *pMode) {
  if (hMonitor != getDefaultMonitor() || !pMode)
    return false;
  uint32_t sw, sh;
  getScreenSize(&sw, &sh);
  fillMode(pMode, sw, sh);
  return true;
}

bool getDesktopDisplayMode(HMONITOR hMonitor, WsiMode *pMode) {
  return getCurrentDisplayMode(hMonitor, pMode);
}

} // namespace dxmt::wsi
