#include "DynamicRangeProcessorHistoryPanel.h"
#include "AColor.h"
#include "AllThemeResources.h"
#include "CompressorInstance.h"
#include "DynamicRangeProcessorHistory.h"
#include "DynamicRangeProcessorOutputs.h"
#include "DynamicRangeProcessorPanelCommon.h"
#include "Theme.h"
#include "widgets/LinearDBFormat.h"
#include "widgets/LinearUpdater.h"
#include "widgets/Ruler.h"
#include <cassert>
#include <wx/dcclient.h>
#include <wx/graphics.h>

namespace
{
constexpr auto defaultHeight = 100;
constexpr auto timerId = 7000;
constexpr auto timerPeriodMs = 1000 / 50;

float GetDbRange(int height)
{
   const auto factor = std::max(
      1.f, 1.f * height / DynamicRangeProcessorHistoryPanel::minHeight);
   return factor * DynamicRangeProcessorHistoryPanel::minRangeDb;
}
} // namespace

BEGIN_EVENT_TABLE(DynamicRangeProcessorHistoryPanel, wxPanelWrapper)
EVT_PAINT(DynamicRangeProcessorHistoryPanel::OnPaint)
EVT_SIZE(DynamicRangeProcessorHistoryPanel::OnSize)
EVT_TIMER(timerId, DynamicRangeProcessorHistoryPanel::OnTimer)
END_EVENT_TABLE()

DynamicRangeProcessorHistoryPanel::DynamicRangeProcessorHistoryPanel(
   wxWindow* parent, wxWindowID winid, DynamicRangeProcessorOutputs& outputs,
   CompressorInstance& instance, std::function<void(float)> onDbRangeChanged)
    : wxPanelWrapper { parent, winid }
    , mOnDbRangeChanged { std::move(onDbRangeChanged) }
    , mInitializeProcessingSettingsSubscription { static_cast<
                                                     InitializeProcessingSettingsPublisher&>(
                                                     instance)
                                                     .Subscribe([this](
                                                                   const auto&
                                                                      evt) {
                                                        if (evt)
                                                           InitializeForPlayback(
                                                              evt->sampleRate);
                                                        else
                                                           // Stop the
                                                           // timer-based
                                                           // update but keep
                                                           // the history
                                                           // until playback
                                                           // is resumed.
                                                           mTimer.Stop();
                                                     }) }
    , mRealtimeResumeSubscription {
       static_cast<RealtimeResumePublisher&>(instance).Subscribe([this](auto) {
          if (mHistory)
             mHistory->BeginNewSegment();
       })
    }
{
   if (const auto& sampleRate = instance.GetSampleRate();
       sampleRate.has_value())
      // Playback is ongoing, and so the `InitializeProcessingSettings` event
      // was already fired.
      InitializeForPlayback(*sampleRate);

   SetDoubleBuffered(true);
   mTimer.SetOwner(this, timerId);
   outputs.SetEditorCallback(
      [&](const std::vector<DynamicRangeProcessorOutputPacket>& packets) {
         if (mHistory)
         {
            mHistory->Push(packets);
            const auto& segments = mHistory->GetSegments();
            if (
               !mSync.has_value() && !segments.empty() &&
               !segments.front().empty())
               mSync.emplace(
                  // Use the first registered packet's time as origin
                  ClockSynchronization { segments.front().front().time,
                                         std::chrono::steady_clock::now() });
         }
         mPlaybackAboutToStart = false;
      });
   SetSize({ -1, defaultHeight });
}

namespace
{
constexpr auto followLineWidth =
   DynamicRangeProcessorPanel::transferFunctionLineWidth + 2;
constexpr auto topMargin = (followLineWidth + 1) / 2;

int GetDisplayPixel(float elapsedSincePacket, int panelWidth)
{
   const auto secondsPerPixel =
      1.f * DynamicRangeProcessorHistory::maxTimeSeconds / panelWidth;
   // A display delay to avoid the display to tremble near time zero because the
   // data hasn't arrived yet.
   // This is a trade-off between visual comfort and timely update. It was set
   // empirically, but with a relatively large audio playback delay. Maybe it
   // will be found to lag on lower-latency playbacks. Best would probably be to
   // make it playback-delay dependent.
   constexpr auto displayDelay = 0.2f;
   return panelWidth - 1 -
          std::round((elapsedSincePacket - 0.2f) / secondsPerPixel);
}

void DrawHistory(
   wxPaintDC& dc, wxGraphicsContext& gc, const wxSize& panelSize,
   const std::vector<DynamicRangeProcessorHistory::Segment>& segments,
   const DynamicRangeProcessorHistoryPanel::ClockSynchronization& sync)
{
   const auto elapsedTimeSinceFirstPacket =
      std::chrono::duration<float>(sync.now - sync.start).count();
   const auto firstPacketTime = sync.firstPacketTime;

   const auto size = panelSize - wxSize { 0, topMargin };
   const auto rangeDb = GetDbRange(size.GetHeight());
   const auto dbPerPixel = rangeDb / size.GetHeight();

   for (const auto& segment : segments)
   {
      if (segment.empty())
         continue;

      std::vector<wxPoint2DDouble> followPoints;
      std::vector<wxPoint2DDouble> targetPoints;
      followPoints.reserve(segment.size());
      targetPoints.reserve(segment.size());
      for (auto it = segment.begin(); it != segment.end(); ++it)
      {
         const auto elapsedSincePacket =
            elapsedTimeSinceFirstPacket - (it->time - firstPacketTime);
         const int x =
            GetDisplayPixel(elapsedSincePacket, panelSize.GetWidth());
         const int yt = topMargin - it->target / dbPerPixel;
         const int yf = topMargin - it->follower / dbPerPixel;
         followPoints.emplace_back(x, yf);
         targetPoints.emplace_back(x, yt);
      }

      gc.SetPen(wxPen { theTheme.Colour(clrResponseLines), followLineWidth });
      if (segment.size() == 1)
      {
         wxInt32 x, y;
         followPoints[0].GetRounded(&x, &y);
         dc.DrawPoint({ x, y });
      }
      else
         gc.DrawLines(followPoints.size(), followPoints.data());

      gc.SetPen(
         wxPen { theTheme.Colour(clrGraphLines),
                 DynamicRangeProcessorPanel::transferFunctionLineWidth });
      if (segment.size() == 1)
      {
         wxInt32 x, y;
         followPoints[0].GetRounded(&x, &y);
         dc.DrawPoint({ x, y });
      }
      else
         gc.DrawLines(targetPoints.size(), targetPoints.data());
   }
}
} // namespace

void DynamicRangeProcessorHistoryPanel::OnPaint(wxPaintEvent& evt)
{
   wxPaintDC dc(this);
   dc.Clear();

   dc.SetBrush(*wxWHITE_BRUSH);
   dc.SetPen(*wxBLACK_PEN);
   dc.DrawRectangle(GetSize());

   if (!mHistory || !mSync)
   {
      if (!mPlaybackAboutToStart)
      {
         const auto text = XO("awaiting playback");
         dc.SetFont(wxFont(
            16, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
         const auto textWidth = dc.GetTextExtent(text.Translation()).GetWidth();
         const auto textHeight =
            dc.GetTextExtent(text.Translation()).GetHeight();
         dc.SetTextForeground(wxColor { 128, 128, 128 });
         dc.DrawText(
            text.Translation(), (GetSize().GetWidth() - textWidth) / 2,
            (GetSize().GetHeight() - textHeight) / 2);
      }
      return;
   }

   const auto& segments = mHistory->GetSegments();
   std::unique_ptr<wxGraphicsContext> gc { wxGraphicsContext::Create(dc) };
   gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
   gc->SetInterpolationQuality(wxINTERPOLATION_BEST);
   DrawHistory(dc, *gc, GetSize(), segments, *mSync);
}

void DynamicRangeProcessorHistoryPanel::OnSize(wxSizeEvent& evt)
{
#ifndef __WXMAC__
   if (mHistory)
      mHistory->BeginNewSegment();
#endif
   Refresh();
   mOnDbRangeChanged(GetDbRange(GetSize().GetHeight()));
}

void DynamicRangeProcessorHistoryPanel::OnTimer(wxTimerEvent& evt)
{
   if (!mSync)
      return;

   // Do now get `std::chrono::steady_clock::now()` in the `OnPaint` event,
   // because this can be triggered even when playback is paused.
   mSync->now = std::chrono::steady_clock::now();

   Refresh(false);
   wxPanelWrapper::Update();
}

void DynamicRangeProcessorHistoryPanel::InitializeForPlayback(double sampleRate)
{
   mSync.reset();
   mHistory.emplace(sampleRate);
   mTimer.Start(timerPeriodMs);
   mPlaybackAboutToStart = true;
   Refresh(false);
   wxPanelWrapper::Update();
}

bool DynamicRangeProcessorHistoryPanel::AcceptsFocus() const
{
   return false;
}

bool DynamicRangeProcessorHistoryPanel::AcceptsFocusFromKeyboard() const
{
   return false;
}
