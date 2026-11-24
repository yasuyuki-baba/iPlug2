/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

#include <cmath>
#include <algorithm>
#include "IPlugAPP_host.h"
#include "config.h"
#include "resource.h"

#include <ctime>

#ifdef OS_WIN
#include "asio.h"
#include "win32_utf8.h"
extern float GetScaleForHWND(HWND hWnd);
#define GET_MENU() GetMenu(gHWND)
extern bool SaveWindowScreenshot(HWND hwnd, const char* path);
#elif defined OS_MAC
#define GET_MENU() SWELL_GetCurrentMenu()
extern "C" bool SaveWindowScreenshot(void* hwnd, const char* path);
#elif defined OS_LINUX
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#if defined IPLUG_EXPERIMENTAL_WAYLAND && !defined NO_IGRAPHICS
#include <gtk/gtk.h>
#endif
#define GET_MENU() GetMenu(gHWND)
static bool SaveWindowScreenshot(HWND, const char*) { return false; }
#endif

using namespace iplug;

#if defined OS_LINUX && defined IPLUG_EXPERIMENTAL_WAYLAND && !defined NO_IGRAPHICS
void IPlugAPPHost::ShowWaylandPreferences(void* parent)
{
  if (!gtk_init_check(nullptr, nullptr))
    return;
  GtkWidget* dialog = gtk_dialog_new_with_buttons("Audio and MIDI Settings", static_cast<GtkWindow*>(parent),
    GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
  auto* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  if (mNoIO)
  {
    gtk_box_pack_start(GTK_BOX(content), gtk_label_new("Audio and MIDI are disabled by --no-io."), FALSE, FALSE, 12);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_OK, FALSE);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  auto* grid = GTK_GRID(gtk_grid_new());
  gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
  gtk_grid_set_row_spacing(grid, 8);
  gtk_grid_set_column_spacing(grid, 12);
  gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(grid), TRUE, TRUE, 0);
  int row = 0;
  const auto addCombo = [&](const char* label) {
    auto* combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    auto* caption = gtk_label_new_with_mnemonic(label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(caption), GTK_WIDGET(combo));
    gtk_widget_set_halign(caption, GTK_ALIGN_START);
    gtk_widget_set_hexpand(GTK_WIDGET(combo), TRUE);
    gtk_grid_attach(grid, caption, 0, row, 1, 1);
    gtk_grid_attach(grid, GTK_WIDGET(combo), 1, row++, 1, 1);
    return combo;
  };
  auto* driver = addCombo("Audio _driver");
  auto* input = addCombo("Audio _input");
  auto* output = addCombo("Audio _output");
  auto* rate = addCombo("Sample _rate");
  auto* buffer = addCombo("Buffer _size");
  auto* midiInput = addCombo("_MIDI input");
  auto* midiOutput = addCombo("MIDI o_utput");
  auto* status = gtk_label_new(nullptr);
  gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
  gtk_grid_attach(grid, status, 0, row, 2, 1);

  const auto select = [](GtkComboBoxText* combo, const char* value) {
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), value))
      gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
  };
  const auto selectNumber = [&select](GtkComboBoxText* combo, uint32_t number) {
    select(combo, std::to_string(number).c_str());
  };
  const auto active = [](GtkComboBoxText* combo) {
    const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    return id ? id : "";
  };
  const auto append = [](GtkComboBoxText* combo, const char* value) {
    gtk_combo_box_text_append(combo, value, value);
  };
  gtk_combo_box_text_append(driver, "0", "ALSA");
  std::vector<RtAudio::Api> apis;
  RtAudio::getCompiledApi(apis);
  if (std::find(apis.begin(), apis.end(), RtAudio::UNIX_JACK) != apis.end())
    gtk_combo_box_text_append(driver, "1", "JACK");
  selectNumber(driver, mState.mAudioDriverType);
  for (const auto& size : kBufferSizeOptions)
    append(buffer, size.c_str());
  selectNumber(buffer, mState.mBufferSize);

  for (auto* combo : {midiInput, midiOutput})
  {
    const auto& devices = combo == midiInput ? mMidiInputDevNames : mMidiOutputDevNames;
    for (const auto& device : devices)
      append(combo, device.c_str());
    if (devices.empty())
      append(combo, OFF_TEXT);
    select(combo, combo == midiInput ? mState.mMidiInDev.Get() : mState.mMidiOutDev.Get());
    gtk_widget_set_sensitive(GTK_WIDGET(combo), !devices.empty());
  }

  std::vector<RtAudio::DeviceInfo> audioDevices;
  std::function<void()> populateRates = [&] {
    const std::string selectedRate = active(rate);
    gtk_combo_box_text_remove_all(rate);
    if (!*active(input) || !*active(output))
      return;
    const auto findDevice = [&](const char* name, bool isInput) {
      return std::find_if(audioDevices.begin(), audioDevices.end(), [&](const RtAudio::DeviceInfo& device) {
        return device.name == name && (isInput ? device.inputChannels : device.outputChannels);
      });
    };
    const auto in = findDevice(active(input), true);
    const auto out = findDevice(active(output), false);
    if (in == audioDevices.end() || out == audioDevices.end())
      return;
    for (uint32_t sampleRate : in->sampleRates)
      if (std::find(out->sampleRates.begin(), out->sampleRates.end(), sampleRate) != out->sampleRates.end())
        append(rate, std::to_string(sampleRate).c_str());
    select(rate, selectedRate.empty() ? std::to_string(mState.mAudioSR).c_str() : selectedRate.c_str());
  };

  std::function<void()> populateDevices = [&] {
    audioDevices.clear();
    RtAudio probe(active(driver)[0] == '1' ? RtAudio::UNIX_JACK : RtAudio::LINUX_ALSA);
    for (auto id : probe.getDeviceIds())
      audioDevices.push_back(probe.getDeviceInfo(id));
    gtk_combo_box_text_remove_all(input);
    gtk_combo_box_text_remove_all(output);
    for (const auto& device : audioDevices)
    {
      if (device.inputChannels)
        append(input, device.name.c_str());
      if (device.outputChannels)
        append(output, device.name.c_str());
    }
    select(input, mState.mAudioInDev.Get());
    select(output, mState.mAudioOutDev.Get());
    populateRates();
  };
  populateDevices();
  auto onChanged = +[](GtkComboBox*, gpointer data) { (*static_cast<std::function<void()>*>(data))(); };
  g_signal_connect(driver, "changed", G_CALLBACK(onChanged), &populateDevices);
  g_signal_connect(input, "changed", G_CALLBACK(onChanged), &populateRates);
  g_signal_connect(output, "changed", G_CALLBACK(onChanged), &populateRates);
  gtk_widget_show_all(dialog);

  while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK)
  {
    const AppState previous(mState);
    AppState next(previous);
    next.mAudioDriverType = active(driver)[0] == '1' ? kDeviceJack : kDeviceAlsa;
    const bool changeDriver = next.mAudioDriverType != previous.mAudioDriverType;
    if (changeDriver || (*active(input) && *active(output)))
    {
      next.mAudioInDev.Set(active(input));
      next.mAudioOutDev.Set(active(output));
      next.mAudioSR = std::strtoul(active(rate), nullptr, 10);
    }
    next.mBufferSize = std::strtoul(active(buffer), nullptr, 10);
    const bool changeAudio = !AudioSettingsInStateAreEqual(previous, next);
    if (changeAudio && (!next.mAudioInDev.GetLength() || !next.mAudioOutDev.GetLength() || !next.mAudioSR))
    {
      gtk_label_set_text(GTK_LABEL(status), "Select an input, output and sample rate.");
      continue;
    }

    mState = next;
    if (changeDriver)
    {
      TryToChangeAudioDriverType();
      ProbeAudioIO();
    }
    if (changeAudio && !TryToChangeAudio(false))
    {
      mState = previous;
      if (changeDriver)
      {
        TryToChangeAudioDriverType();
        ProbeAudioIO();
      }
      TryToChangeAudio(false);
      UpdateINI();
      gtk_label_set_text(GTK_LABEL(status), "Could not start audio with these settings.");
      continue;
    }

    if (mMidiIn && strcmp(active(midiInput), previous.mMidiInDev.Get()) != 0)
    {
      mState.mMidiInDev.Set(active(midiInput));
      SelectMIDIDevice(ERoute::kInput, mState.mMidiInDev.Get());
    }
    if (mMidiOut && strcmp(active(midiOutput), previous.mMidiOutDev.Get()) != 0)
    {
      mState.mMidiOutDev.Set(active(midiOutput));
      SelectMIDIDevice(ERoute::kOutput, mState.mMidiOutDev.Get());
    }
    UpdateINI();
    break;
  }

  g_signal_handlers_disconnect_by_data(driver, &populateDevices);
  g_signal_handlers_disconnect_by_data(input, &populateRates);
  g_signal_handlers_disconnect_by_data(output, &populateRates);
  gtk_widget_destroy(dialog);
}
#endif

#if !defined NO_IGRAPHICS
#include "IGraphics.h"
using namespace igraphics;
#endif

#define IDT_SCREENSHOT_TIMER 1001
#ifdef OS_LINUX
#define IDT_OPENWINDOW_TIMER 1002
static bool sPlugWindowOpen = false; // guards WM_SIZE before plugin window is ready

// SWELL does not position the modal dialogs it creates, and window managers
// that do not place transient windows themselves leave them at the screen
// origin. Call this before opening a modal dialog owned by pOwner: once the
// modal loop is running, a one-shot timer centres the dialog over its owner.
#define IDT_CENTRE_DIALOG_TIMER 1003
static void CALLBACK CentreOwnedDialogs(HWND pOwner, UINT, UINT_PTR timerID, DWORD)
{
  KillTimer(pOwner, timerID);
  EnumWindows([](HWND pWnd, LPARAM lParam) -> BOOL {
    HWND pOwner = reinterpret_cast<HWND>(lParam);
    if (pWnd != pOwner && GetWindow(pWnd, GW_OWNER) == pOwner && IsWindowVisible(pWnd))
    {
      RECT owner, dialog;
      GetWindowRect(pOwner, &owner);
      GetWindowRect(pWnd, &dialog);
      const int x = owner.left + ((owner.right - owner.left) - (dialog.right - dialog.left)) / 2;
      const int y = owner.top + ((owner.bottom - owner.top) - (dialog.bottom - dialog.top)) / 2;
      SetWindowPos(pWnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(pOwner));
}

static void CentreNextOwnedDialog(HWND pOwner)
{
  SetTimer(pOwner, IDT_CENTRE_DIALOG_TIMER, 1, CentreOwnedDialogs);
}
#else
static void CentreNextOwnedDialog(HWND) {}
#endif


// check the input and output devices, find matching srs
void IPlugAPPHost::PopulateSampleRateList(HWND hwndDlg, RtAudio::DeviceInfo* inputDevInfo, RtAudio::DeviceInfo* outputDevInfo)
{
  WDL_String buf;

  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_SR,CB_RESETCONTENT,0,0);

  std::vector<int> matchedSRs;

  for (int i=0; i<inputDevInfo->sampleRates.size(); i++)
  {
    for (int j=0; j<outputDevInfo->sampleRates.size(); j++)
    {
      if (inputDevInfo->sampleRates[i] == outputDevInfo->sampleRates[j])
        matchedSRs.push_back(inputDevInfo->sampleRates[i]);
    }
  }

  for (int k=0; k<matchedSRs.size(); k++)
  {
    buf.SetFormatted(20, "%i", matchedSRs[k]);
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_SR,CB_ADDSTRING,0,(LPARAM)buf.Get());
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_SR,CB_SETITEMDATA,k,(LPARAM)matchedSRs[k]);
  }
  
  WDL_String str;
  str.SetFormatted(32, "%i", mState.mAudioSR);

  LRESULT sridx = SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_SR, CB_FINDSTRINGEXACT, -1, (LPARAM) str.Get());
  SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_SR, CB_SETCURSEL, sridx, 0);
}

void IPlugAPPHost::PopulateAudioInputList(HWND hwndDlg, RtAudio::DeviceInfo* info)
{
  WDL_String buf;

  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_L,CB_RESETCONTENT,0,0);
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_R,CB_RESETCONTENT,0,0);

  int i;

  for (i=0; i<info->inputChannels -1; i++)
  {
    buf.SetFormatted(20, "%i", i+1);
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_L,CB_ADDSTRING,0,(LPARAM)buf.Get());
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_R,CB_ADDSTRING,0,(LPARAM)buf.Get());
  }

  // TEMP
  buf.SetFormatted(20, "%i", i+1);
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_R,CB_ADDSTRING,0,(LPARAM)buf.Get());

  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_L,CB_SETCURSEL, mState.mAudioInChanL - 1, 0);
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_R,CB_SETCURSEL, mState.mAudioInChanR - 1, 0);
}

void IPlugAPPHost::PopulateAudioOutputList(HWND hwndDlg, RtAudio::DeviceInfo* info)
{
  WDL_String buf;

  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_L,CB_RESETCONTENT,0,0);
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_R,CB_RESETCONTENT,0,0);

  int i;

  for (i=0; i<info->outputChannels -1; i++)
  {
    buf.SetFormatted(20, "%i", i+1);
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_L,CB_ADDSTRING,0,(LPARAM)buf.Get());
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_R,CB_ADDSTRING,0,(LPARAM)buf.Get());
  }

  // TEMP
  buf.SetFormatted(20, "%i", i+1);
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_R,CB_ADDSTRING,0,(LPARAM)buf.Get());

  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_L,CB_SETCURSEL, mState.mAudioOutChanL - 1, 0);
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_R,CB_SETCURSEL, mState.mAudioOutChanR - 1, 0);
}

// This has to get called after any change to audio driver/in dev/out dev
void IPlugAPPHost::PopulateDriverSpecificControls(HWND hwndDlg)
{
#ifdef OS_WIN
  int driverType = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_DRIVER, CB_GETCURSEL, 0, 0);
  if (driverType == kDeviceASIO)
  {
    ComboBox_Enable(GetDlgItem(hwndDlg, IDC_COMBO_AUDIO_IN_DEV), FALSE);
    Button_Enable(GetDlgItem(hwndDlg, IDC_BUTTON_OS_DEV_SETTINGS), TRUE);
  }
  else
  {
    ComboBox_Enable(GetDlgItem(hwndDlg, IDC_COMBO_AUDIO_IN_DEV), TRUE);
    Button_Enable(GetDlgItem(hwndDlg, IDC_BUTTON_OS_DEV_SETTINGS), FALSE);
  }
#endif

  int indevidx = 0;
  int outdevidx = 0;

  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_DEV,CB_RESETCONTENT,0,0);
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_DEV,CB_RESETCONTENT,0,0);

  for (int i = 0; i<mAudioInputDevIDs.size(); i++)
  {
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_DEV,CB_ADDSTRING,0,(LPARAM)GetAudioDeviceName(mAudioInputDevIDs[i]).c_str());

    if (std::string_view(GetAudioDeviceName(mAudioInputDevIDs[i])) == mState.mAudioInDev.Get())
      indevidx = i;
  }

  for (int i = 0; i<mAudioOutputDevIDs.size(); i++)
  {
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_DEV,CB_ADDSTRING,0,(LPARAM)GetAudioDeviceName(mAudioOutputDevIDs[i]).c_str());

    if (std::string_view(GetAudioDeviceName(mAudioOutputDevIDs[i])) == mState.mAudioOutDev.Get())
      outdevidx = i;
  }

#ifdef OS_WIN
  if (driverType == kDeviceASIO)
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_DEV,CB_SETCURSEL, outdevidx, 0);
  else
#endif
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_DEV,CB_SETCURSEL, indevidx, 0);

  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_DEV,CB_SETCURSEL, outdevidx, 0);

  RtAudio::DeviceInfo inputDevInfo;
  RtAudio::DeviceInfo outputDevInfo;

  if (mAudioInputDevIDs.size())
  {
    inputDevInfo = mDAC->getDeviceInfo(mAudioInputDevIDs[indevidx]);
    PopulateAudioInputList(hwndDlg, &inputDevInfo);
  }

  if (mAudioOutputDevIDs.size())
  {
    outputDevInfo = mDAC->getDeviceInfo(mAudioOutputDevIDs[outdevidx]);
    PopulateAudioOutputList(hwndDlg, &outputDevInfo);
  }

  PopulateSampleRateList(hwndDlg, &inputDevInfo, &outputDevInfo);
}

void IPlugAPPHost::PopulateAudioDialogs(HWND hwndDlg)
{
  PopulateDriverSpecificControls(hwndDlg);

//  if (mState.mAudioInIsMono)
//  {
//    SendDlgItemMessage(hwndDlg,IDC_CB_MONO_INPUT,BM_SETCHECK, BST_CHECKED,0);
//  }
//  else
//  {
//    SendDlgItemMessage(hwndDlg,IDC_CB_MONO_INPUT,BM_SETCHECK, BST_UNCHECKED,0);
//  }

//  Populate buffer size combobox
  for (int i = 0; i< kNumBufferSizeOptions; i++)
  {
    SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_BUF_SIZE,CB_ADDSTRING,0,(LPARAM)kBufferSizeOptions[i].c_str());
  }
  
  WDL_String str;
  str.SetFormatted(32, "%i", mState.mBufferSize);

  LRESULT iovsidx = SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_BUF_SIZE, CB_FINDSTRINGEXACT, -1, (LPARAM) str.Get());
  SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_BUF_SIZE, CB_SETCURSEL, iovsidx, 0);
}

bool IPlugAPPHost::PopulateMidiDialogs(HWND hwndDlg)
{
  if ( !mMidiIn || !mMidiOut )
    return false;
  else
  {
    for (int i=0; i<mMidiInputDevNames.size(); i++)
    {
      SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_IN_DEV,CB_ADDSTRING,0,(LPARAM)mMidiInputDevNames[i].c_str());
    }

    LRESULT indevidx = SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_IN_DEV,CB_FINDSTRINGEXACT, -1, (LPARAM)mState.mMidiInDev.Get());

    // if the midi port name wasn't found update the ini file, and set to off
    if (indevidx == -1)
    {
      mState.mMidiInDev.Set("off");
      UpdateINI();
      indevidx = 0;
    }

    SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_IN_DEV,CB_SETCURSEL, indevidx, 0);

    for (int i=0; i<mMidiOutputDevNames.size(); i++)
    {
      SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_OUT_DEV,CB_ADDSTRING,0,(LPARAM)mMidiOutputDevNames[i].c_str());
    }

    LRESULT outdevidx = SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_OUT_DEV,CB_FINDSTRINGEXACT, -1, (LPARAM)mState.mMidiOutDev.Get());

    // if the midi port name wasn't found update the ini file, and set to off
    if (outdevidx == -1)
    {
      mState.mMidiOutDev.Set("off");
      UpdateINI();
      outdevidx = 0;
    }

    SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_OUT_DEV,CB_SETCURSEL, outdevidx, 0);

    // Populate MIDI channel dialogs

    SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_IN_CHAN,CB_ADDSTRING,0,(LPARAM)"all");
    SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_OUT_CHAN,CB_ADDSTRING,0,(LPARAM)"all");

    WDL_String buf;

    for (int i=0; i<16; i++)
    {
      buf.SetFormatted(20, "%i", i+1);
      SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_IN_CHAN,CB_ADDSTRING,0,(LPARAM)buf.Get());
      SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_OUT_CHAN,CB_ADDSTRING,0,(LPARAM)buf.Get());
    }

    SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_IN_CHAN,CB_SETCURSEL, (LPARAM)mState.mMidiInChan, 0);
    SendDlgItemMessage(hwndDlg,IDC_COMBO_MIDI_OUT_CHAN,CB_SETCURSEL, (LPARAM)mState.mMidiOutChan, 0);

    return true;
  }
}

#ifdef OS_WIN
void IPlugAPPHost::PopulatePreferencesDialog(HWND hwndDlg)
{
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_ADDSTRING,0,(LPARAM)"DirectSound");
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_ADDSTRING,0,(LPARAM)"ASIO");
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_SETCURSEL, mState.mAudioDriverType, 0);

  PopulateAudioDialogs(hwndDlg);
  PopulateMidiDialogs(hwndDlg);
}

#elif defined OS_MAC
void IPlugAPPHost::PopulatePreferencesDialog(HWND hwndDlg)
{
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_ADDSTRING,0,(LPARAM)"CoreAudio");
  //SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_ADDSTRING,0,(LPARAM)"Jack");
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_SETCURSEL, mState.mAudioDriverType, 0);

  PopulateAudioDialogs(hwndDlg);
  PopulateMidiDialogs(hwndDlg);
}
#elif defined OS_LINUX
void IPlugAPPHost::PopulatePreferencesDialog(HWND hwndDlg)
{
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_ADDSTRING,0,(LPARAM)"ALSA");
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_ADDSTRING,0,(LPARAM)"JACK");
  SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_DRIVER,CB_SETCURSEL, mState.mAudioDriverType, 0);

  PopulateAudioDialogs(hwndDlg);
  PopulateMidiDialogs(hwndDlg);
}
#endif

WDL_DLGRET IPlugAPPHost::PreferencesDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  IPlugAPPHost* _this = sInstance.get();
  AppState& mState = _this->mState;
  AppState& mTempState = _this->mTempState;
  AppState& mActiveState = _this->mActiveState;

  auto getComboString = [&](WDL_String& str, int item, WPARAM idx) {
    std::string tempString;
    long len = (long) SendDlgItemMessage(hwndDlg, item, CB_GETLBTEXTLEN, idx, 0) + 1;
    tempString.reserve(len);
    SendDlgItemMessage(hwndDlg, item, CB_GETLBTEXT, idx, (LPARAM) tempString.data());
    str.Set(tempString.c_str());
  };
  
  int v = 0;
  switch(uMsg)
  {
    case WM_INITDIALOG:
#ifdef OS_WIN
      WDL_UTF8_HookComboBox(GetDlgItem(hwndDlg, IDC_COMBO_AUDIO_IN_DEV));
      WDL_UTF8_HookComboBox(GetDlgItem(hwndDlg, IDC_COMBO_AUDIO_OUT_DEV));
      WDL_UTF8_HookComboBox(GetDlgItem(hwndDlg, IDC_COMBO_MIDI_IN_DEV));
      WDL_UTF8_HookComboBox(GetDlgItem(hwndDlg, IDC_COMBO_MIDI_OUT_DEV));
#endif
      _this->PopulatePreferencesDialog(hwndDlg);
      mTempState = mState;
      
      return TRUE;

    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDOK:
          if (mActiveState != mState)
            _this->TryToChangeAudio();

          EndDialog(hwndDlg, IDOK); // INI file will be changed see MainDialogProc
          break;
        case IDAPPLY:
          _this->TryToChangeAudio();
          break;
        case IDCANCEL:
          EndDialog(hwndDlg, IDCANCEL);

          // if state has been changed reset to previous state, INI file won't be changed
          if (!_this->AudioSettingsInStateAreEqual(mState, mTempState)
              || !_this->MIDISettingsInStateAreEqual(mState, mTempState))
          {
            mState = mTempState;

            _this->TryToChangeAudioDriverType();
            _this->ProbeAudioIO();
            _this->TryToChangeAudio();
          }

          break;

        case IDC_COMBO_AUDIO_DRIVER:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            v = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_DRIVER, CB_GETCURSEL, 0, 0);

            if (v != mState.mAudioDriverType)
            {
              mState.mAudioDriverType = v;

              _this->TryToChangeAudioDriverType();
              _this->ProbeAudioIO();

              if (_this->mAudioInputDevIDs.size())
                mState.mAudioInDev.Set(_this->GetAudioDeviceName(_this->mAudioInputDevIDs[0]).c_str());

              if (_this->mAudioOutputDevIDs.size())
                mState.mAudioOutDev.Set(_this->GetAudioDeviceName(_this->mAudioOutputDevIDs[0]).c_str());

              // Reset IO
              mState.mAudioOutChanL = 1;
              mState.mAudioOutChanR = 2;

              _this->PopulateAudioDialogs(hwndDlg);
            }
          }
          break;

        case IDC_COMBO_AUDIO_IN_DEV:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            int idx = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_IN_DEV, CB_GETCURSEL, 0, 0);
            getComboString(mState.mAudioInDev, IDC_COMBO_AUDIO_IN_DEV, idx);

            // Reset IO
            mState.mAudioInChanL = 1;
            mState.mAudioInChanR = 2;

            _this->PopulateDriverSpecificControls(hwndDlg);
          }
          break;

        case IDC_COMBO_AUDIO_OUT_DEV:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            int idx = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_OUT_DEV, CB_GETCURSEL, 0, 0);
            getComboString(mState.mAudioOutDev, IDC_COMBO_AUDIO_OUT_DEV, idx);

            // Reset IO
            mState.mAudioOutChanL = 1;
            mState.mAudioOutChanR = 2;

            _this->PopulateDriverSpecificControls(hwndDlg);
          }
          break;

        case IDC_COMBO_AUDIO_IN_L:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            mState.mAudioInChanL = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_IN_L, CB_GETCURSEL, 0, 0) + 1;

            //TEMP
            mState.mAudioInChanR = mState.mAudioInChanL + 1;
            SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_R,CB_SETCURSEL, mState.mAudioInChanR - 1, 0);
            //
          }
          break;

        case IDC_COMBO_AUDIO_IN_R:
          if (HIWORD(wParam) == CBN_SELCHANGE)
            SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_IN_R,CB_SETCURSEL, mState.mAudioInChanR - 1, 0);  // TEMP
                mState.mAudioInChanR = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_IN_R, CB_GETCURSEL, 0, 0);
          break;

        case IDC_COMBO_AUDIO_OUT_L:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            mState.mAudioOutChanL = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_OUT_L, CB_GETCURSEL, 0, 0) + 1;

            //TEMP
            mState.mAudioOutChanR = mState.mAudioOutChanL + 1;
            SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_R,CB_SETCURSEL, mState.mAudioOutChanR - 1, 0);
            //
          }
          break;

        case IDC_COMBO_AUDIO_OUT_R:
          if (HIWORD(wParam) == CBN_SELCHANGE)
            SendDlgItemMessage(hwndDlg,IDC_COMBO_AUDIO_OUT_R,CB_SETCURSEL, mState.mAudioOutChanR - 1, 0);  // TEMP
                mState.mAudioOutChanR = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_OUT_R, CB_GETCURSEL, 0, 0);
          break;

//        case IDC_CB_MONO_INPUT:
//          if (SendDlgItemMessage(hwndDlg,IDC_CB_MONO_INPUT, BM_GETCHECK, 0, 0) == BST_CHECKED)
//            mState.mAudioInIsMono = 1;
//          else
//            mState.mAudioInIsMono = 0;
//          break;

        case IDC_COMBO_AUDIO_BUF_SIZE: // follow through
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            int iovsidx = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_BUF_SIZE, CB_GETCURSEL, 0, 0);
            mState.mBufferSize = atoi(kBufferSizeOptions[iovsidx].c_str());
          }
          break;
        case IDC_COMBO_AUDIO_SR:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            int idx = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_SR, CB_GETCURSEL, 0, 0);
            mState.mAudioSR = (uint32_t) SendDlgItemMessage(hwndDlg, IDC_COMBO_AUDIO_SR, CB_GETITEMDATA, idx, 0);
          }
          break;

        case IDC_BUTTON_OS_DEV_SETTINGS:
          if (HIWORD(wParam) == BN_CLICKED)
          {
            #ifdef OS_WIN
            if ( (_this->mState.mAudioDriverType == kDeviceASIO) && (_this->mDAC->isStreamRunning() == true)) // TODO: still not right
              ASIOControlPanel();
            #elif defined OS_MAC
            if (SWELL_GetOSXVersion() >= 0x1200)
            {
              system("open \"/System/Applications/Utilities/Audio MIDI Setup.app\"");
            }
            else
            {
              system("open \"/Applications/Utilities/Audio MIDI Setup.app\"");
            }
            #elif defined OS_LINUX
            {
              // Double-fork so the launched process is reparented to init and the
              // intermediate child is reaped here — no zombie accumulation.
              pid_t pid = fork();
              if (pid == 0)
              {
                pid_t grandchild = fork();
                if (grandchild == 0)
                {
                  execlp("xdg-open", "xdg-open", "https://alsa-project.org", (char*)nullptr);
                  _exit(127);
                }
                _exit(grandchild > 0 ? 0 : 1);
              }
              if (pid > 0)
              {
                int status;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
              }
            }
            #endif
          }
          break;

        case IDC_COMBO_MIDI_IN_DEV:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            int idx = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_MIDI_IN_DEV, CB_GETCURSEL, 0, 0);
            getComboString(mState.mMidiInDev, IDC_COMBO_MIDI_IN_DEV, idx);
            _this->SelectMIDIDevice(ERoute::kInput, mState.mMidiInDev.Get());
          }
          break;

        case IDC_COMBO_MIDI_OUT_DEV:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            int idx = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_MIDI_OUT_DEV, CB_GETCURSEL, 0, 0);
            getComboString(mState.mMidiOutDev, IDC_COMBO_MIDI_OUT_DEV, idx);
            _this->SelectMIDIDevice(ERoute::kOutput, mState.mMidiOutDev.Get());
          }
          break;

        case IDC_COMBO_MIDI_IN_CHAN:
          if (HIWORD(wParam) == CBN_SELCHANGE)
            mState.mMidiInChan = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_MIDI_IN_CHAN, CB_GETCURSEL, 0, 0);
          break;

        case IDC_COMBO_MIDI_OUT_CHAN:
          if (HIWORD(wParam) == CBN_SELCHANGE)
            mState.mMidiOutChan = (int) SendDlgItemMessage(hwndDlg, IDC_COMBO_MIDI_OUT_CHAN, CB_GETCURSEL, 0, 0);
          break;

        default:
          break;
      }
      break;
    default:
      return FALSE;
  }
  return TRUE;
}

static void ClientResize(HWND hWnd, int width, int height)
{
  RECT rcClient, rcWindow;
  POINT ptDiff;
  int screenwidth, screenheight;
  int x, y;
  
#ifdef OS_LINUX
  // The container is sized in GDK units; convert from editor units, which
  // differ when IPLUG2_SCREEN_SCALE overrides GDK's integer scale factor.
  const float containerScale = IPlugAPPHost::ResolveLinuxScreenScale(hWnd);
  width = static_cast<int>(std::lround(width * containerScale));
  height = static_cast<int>(std::lround(height * containerScale));
#endif

  screenwidth  = GetSystemMetrics(SM_CXSCREEN);
  screenheight = GetSystemMetrics(SM_CYSCREEN);
  x = (screenwidth / 2) - (width / 2);
  y = (screenheight / 2) - (height / 2);
  
  GetClientRect(hWnd, &rcClient);
  GetWindowRect(hWnd, &rcWindow);

  ptDiff.x = (rcWindow.right - rcWindow.left) - rcClient.right;
  ptDiff.y = (rcWindow.bottom - rcWindow.top) - rcClient.bottom;
  
  SetWindowPos(hWnd, 0, x, y, width + ptDiff.x, height + ptDiff.y, 0);
}

//static
WDL_DLGRET IPlugAPPHost::MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  IPlugAPPHost* pAppHost = IPlugAPPHost::sInstance.get();

  switch (uMsg)
  {
    case WM_INITDIALOG:
    {
      gHWND = hwndDlg;
      IPlugAPP* pPlug = pAppHost->GetPlug();

#ifdef OS_LINUX
      // On Linux, show the dialog but don't resize or open the window yet.
      // main() attaches any menu first, then posts WM_USER_OPENWINDOW which
      // triggers ClientResize + OpenWindow so the SWELL layout includes
      // the menu bar before the plugin window is created.
      ShowWindow(hwndDlg, SW_SHOW);
#else
      if (!pAppHost->OpenWindow(gHWND))
      {
        DBGMSG("couldn't attach gui\n");
      }

      ClientResize(hwndDlg, pPlug->GetEditorWidth(), pPlug->GetEditorHeight());

      ShowWindow(hwndDlg, SW_SHOW);
#endif

      // If in screenshot mode, start timer to take screenshot after UI initializes
      if (pAppHost->IsScreenshotMode())
      {
        SetTimer(hwndDlg, IDT_SCREENSHOT_TIMER, 500, nullptr); // 500ms delay
      }

      return 1;
    }
#ifdef OS_LINUX
    case WM_USER_OPENWINDOW:
    {
      // Posted from main() after SetMenu. Use a short timer to let the
      // GDK event loop process the SetMenu resize before we size and
      // open the plugin window.
      if (!sPlugWindowOpen && !SetTimer(hwndDlg, IDT_OPENWINDOW_TIMER, 50, nullptr))
      {
        fprintf(stderr, "Could not schedule standalone editor initialization\n");
        gStandaloneExitStatus = EXIT_FAILURE;
        DestroyWindow(hwndDlg);
      }
      return 0;
    }
#endif
    case WM_TIMER:
    {
#ifdef OS_LINUX
      if (wParam == IDT_OPENWINDOW_TIMER)
      {
        KillTimer(hwndDlg, IDT_OPENWINDOW_TIMER);
        if (sPlugWindowOpen)
          return 0;
        IPlugAPP* pPlug = pAppHost->GetPlug();
        ClientResize(hwndDlg, pPlug->GetEditorWidth(), pPlug->GetEditorHeight());
        if (!pAppHost->OpenWindow(gHWND))
        {
          fprintf(stderr, "Could not attach the standalone editor\n");
          gStandaloneExitStatus = EXIT_FAILURE;
          DestroyWindow(hwndDlg);
          return 0;
        }
        sPlugWindowOpen = true;
        return 0;
      }
#endif
      if (wParam == IDT_SCREENSHOT_TIMER)
      {
        KillTimer(hwndDlg, IDT_SCREENSHOT_TIMER);

        SaveWindowScreenshot(gHWND, pAppHost->GetScreenshotPath());

        // Exit the application
#ifdef OS_LINUX
        gStandaloneCloseRequested = true;
#else
        DestroyWindow(hwndDlg);
#endif
        return 0;
      }
      break;
    }
    case WM_DESTROY:
      pAppHost->CloseWindow();
      gHWND = NULL;
#ifdef OS_LINUX
      sPlugWindowOpen = false;
#endif
      IPlugAPPHost::sInstance = nullptr;
      
      #ifdef OS_WIN
      PostQuitMessage(0);
      #elif defined OS_MAC
      SWELL_PostQuitMessage(hwndDlg);
      #endif
      // Linux: main loop exits when gHWND->m_hashaddestroy is set by DestroyWindow

      return 0;
    case WM_CLOSE:
#ifdef OS_LINUX
      gStandaloneCloseRequested = true;
#else
      DestroyWindow(hwndDlg);
#endif
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case ID_QUIT:
        {
#ifdef OS_LINUX
          gStandaloneCloseRequested = true;
#else
          DestroyWindow(hwndDlg);
#endif
          return 0;
        }
        case ID_ABOUT:
        {
          IPlugAPP* pPlug = pAppHost->GetPlug();
          
          bool pluginOpensAboutBox = pPlug->OnHostRequestingAboutBox();
          
          if (pluginOpensAboutBox == false)
          {
            WDL_String info;
            info.Append(PLUG_COPYRIGHT_STR"\nBuilt on " __DATE__);
            CentreNextOwnedDialog(hwndDlg);
            MessageBox(hwndDlg, info.Get(), PLUG_NAME, MB_OK);
          }

          return 0;
        }
        case ID_HELP:
        {
          IPlugAPP* pPlug = pAppHost->GetPlug();

          bool pluginOpensHelp = pPlug->OnHostRequestingProductHelp();

          if (pluginOpensHelp == false)
          {
            CentreNextOwnedDialog(hwndDlg);
            MessageBox(hwndDlg, "See the manual", PLUG_NAME, MB_OK);
          }
          return 0;
        }
        case ID_PREFERENCES:
        {
          CentreNextOwnedDialog(hwndDlg);
          INT_PTR ret = DialogBox(gHINSTANCE, MAKEINTRESOURCE(IDD_DIALOG_PREF), hwndDlg, IPlugAPPHost::PreferencesDlgProc);

          if (ret == IDOK)
            pAppHost->UpdateINI();

          return 0;
        }
#ifdef ID_SCREENSHOT
        case ID_SCREENSHOT:
        {
          // Generate filename with timestamp
          WDL_String path;
          char timestamp[32];
          time_t now = time(nullptr);
          strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", localtime(&now));

          // Get temp directory
          #ifdef OS_WIN
          char tempPath[MAX_PATH];
          GetTempPathA(MAX_PATH, tempPath);
          path.SetFormatted(512, "%s%s_screenshot_%s.png",
                            tempPath,
                            PLUG_NAME,
                            timestamp);
          #elif defined OS_MAC
          const char* tmpDir = getenv("TMPDIR");
          path.SetFormatted(512, "%s%s_screenshot_%s.png",
                            tmpDir ? tmpDir : "/tmp/",
                            PLUG_NAME,
                            timestamp);
          #endif

          if (SaveWindowScreenshot(gHWND, path.Get()))
          {
            WDL_String msg;
            msg.SetFormatted(512, "Screenshot saved to:\n%s\n\nOpen it?", path.Get());
            int result = MessageBox(hwndDlg, msg.Get(), "Screenshot Saved", MB_YESNO);

            if (result == IDYES)
            {
              #ifdef OS_WIN
              ShellExecuteA(NULL, "open", path.Get(), NULL, NULL, SW_SHOWNORMAL);
              #elif defined OS_MAC
              WDL_String cmd;
              cmd.SetFormatted(1024, "open \"%s\"", path.Get());
              system(cmd.Get());
              #endif
            }
          }
          else
          {
            MessageBox(hwndDlg, "Failed to save screenshot", "Error", MB_OK);
          }

          return 0;
        }
#endif
#if defined _DEBUG && !defined NO_IGRAPHICS
        case ID_LIVE_EDIT:
        {
          IGEditorDelegate* pPlug = dynamic_cast<IGEditorDelegate*>(pAppHost->GetPlug());
        
          if (pPlug)
          {
            IGraphics* pGraphics = pPlug->GetUI();
            
            if (pGraphics)
            {
              bool enabled = pGraphics->LiveEditEnabled();
              pGraphics->EnableLiveEdit(!enabled);
              CheckMenuItem(GET_MENU(), ID_LIVE_EDIT, (MF_BYCOMMAND | enabled) ? MF_UNCHECKED : MF_CHECKED);
            }
          }
          
          return 0;
        }
        case ID_SHOW_DRAWN:
        {
          IGEditorDelegate* pPlug = dynamic_cast<IGEditorDelegate*>(pAppHost->GetPlug());
          
          if (pPlug)
          {
            IGraphics* pGraphics = pPlug->GetUI();
            
            if (pGraphics)
            {
              bool enabled = pGraphics->ShowAreaDrawnEnabled();
              pGraphics->ShowAreaDrawn(!enabled);
              CheckMenuItem(GET_MENU(), ID_SHOW_DRAWN, (MF_BYCOMMAND | enabled) ? MF_UNCHECKED : MF_CHECKED);
            }
          }
          
          return 0;
        }
        case ID_SHOW_BOUNDS:
        {
          IGEditorDelegate* pPlug = dynamic_cast<IGEditorDelegate*>(pAppHost->GetPlug());
          
          if (pPlug)
          {
            IGraphics* pGraphics = pPlug->GetUI();
            
            if (pGraphics)
            {
              bool enabled = pGraphics->ShowControlBoundsEnabled();
              pGraphics->ShowControlBounds(!enabled);
              CheckMenuItem(GET_MENU(), ID_SHOW_BOUNDS, (MF_BYCOMMAND | enabled) ? MF_UNCHECKED : MF_CHECKED);
            }
          }
          
          return 0;
        }
        case ID_SHOW_FPS:
        {
          IGEditorDelegate* pPlug = dynamic_cast<IGEditorDelegate*>(pAppHost->GetPlug());
          
          if (pPlug)
          {
            IGraphics* pGraphics = pPlug->GetUI();
            
            if (pGraphics)
            {
              bool enabled = pGraphics->ShowingFPSDisplay();
              pGraphics->ShowFPSDisplay(!enabled);
              CheckMenuItem(GET_MENU(), ID_SHOW_FPS, (MF_BYCOMMAND | enabled) ? MF_UNCHECKED : MF_CHECKED);
            }
          }
          
          return 0;
        }
#endif
      }
      return 0;
    case WM_GETMINMAXINFO:
    {
      if (!pAppHost)
        return 1;
      
      IPlugAPP* pPlug = pAppHost->GetPlug();

      MINMAXINFO* mmi = (MINMAXINFO*) lParam;
      mmi->ptMinTrackSize.x = pPlug->GetMinWidth();
      mmi->ptMinTrackSize.y = pPlug->GetMinHeight();
      mmi->ptMaxTrackSize.x = pPlug->GetMaxWidth();
      mmi->ptMaxTrackSize.y = pPlug->GetMaxHeight();

#ifdef OS_WIN 
      float scale = GetScaleForHWND(hwndDlg);
      mmi->ptMinTrackSize.x = static_cast<LONG>(static_cast<float>(mmi->ptMinTrackSize.x) * scale);
      mmi->ptMinTrackSize.y = static_cast<LONG>(static_cast<float>(mmi->ptMinTrackSize.y) * scale);
      mmi->ptMaxTrackSize.x = static_cast<LONG>(static_cast<float>(mmi->ptMaxTrackSize.x) * scale);
      mmi->ptMaxTrackSize.y = static_cast<LONG>(static_cast<float>(mmi->ptMaxTrackSize.y) * scale);
#endif
      
      return 0;
    }
#ifdef OS_WIN
    case WM_DPICHANGED:
    {
      WORD dpi = HIWORD(wParam);
      RECT* rect = (RECT*)lParam;
      float scale = GetScaleForHWND(hwndDlg);

      POINT ptDiff;
      RECT rcClient;
      RECT rcWindow;

      GetClientRect(hwndDlg, &rcClient);
      GetWindowRect(hwndDlg, &rcWindow);

      ptDiff.x = (rcWindow.right - rcWindow.left) - rcClient.right;
      ptDiff.y = (rcWindow.bottom - rcWindow.top) - rcClient.bottom;

#ifndef NO_IGRAPHICS
      IGEditorDelegate* pPlug = dynamic_cast<IGEditorDelegate*>(pAppHost->GetPlug());

      if (pPlug)
      {
        IGraphics* pGraphics = pPlug->GetUI();

        if (pGraphics)
        {
          pGraphics->SetScreenScale(scale);
        }
      }
#else
      IEditorDelegate* pPlug = dynamic_cast<IEditorDelegate*>(pAppHost->GetPlug());
#endif

      int w = pPlug->GetEditorWidth(); 
      int h = pPlug->GetEditorHeight();

      SetWindowPos(hwndDlg, 0, rect->left, rect->top, w + ptDiff.x, h + ptDiff.y, 0);

      return 0;
    }
#endif
    case WM_SIZE:
    {
      IPlugAPP* pPlug = pAppHost->GetPlug();

      switch (LOWORD(wParam))
      {
      case SIZE_RESTORED:
      case SIZE_MAXIMIZED:
      {
#ifdef OS_LINUX
        // Ignore WM_SIZE events that fire before the plugin window opens.
        // ShowWindow + ClientResize generate spurious resize events (including
        // the dialog template size) that would recreate the FBO multiple times.
        if (!sPlugWindowOpen)
          return 0;
#endif
        if (pPlug->GetHostResizeEnabled())
        {
          RECT r;
          GetClientRect(hwndDlg, &r);
          pPlug->OnParentWindowResize(static_cast<int>(r.right), static_cast<int>(r.bottom));
        }
        return 1;
      }
      default:
        return 0;
      }
    }
  }
  return 0;
}
