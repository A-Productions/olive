/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2019 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "viewer.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QResizeEvent>
#include <QScreen>
#include <QtMath>
#include <QVBoxLayout>

#include "audio/audiomanager.h"
#include "common/power.h"
#include "common/ratiodialog.h"
#include "common/timecodefunctions.h"
#include "config/config.h"
#include "project/item/sequence/sequence.h"
#include "project/project.h"
#include "render/pixelformat.h"
#include "task/taskmanager.h"
#include "widget/menu/menu.h"
#include "window/mainwindow/mainwindow.h"

OLIVE_NAMESPACE_ENTER

const int kMaxPreQueueSize = 16;

CacheTask* ViewerWidget::cache_background_task_ = nullptr;

int ViewerWidget::busy_viewers_ = 0;

ViewerWidget::ViewerWidget(QWidget *parent) :
  TimeBasedWidget(false, true, parent),
  playback_speed_(0),
  frame_cache_job_time_(0),
  color_menu_enabled_(true),
  override_color_manager_(nullptr),
  time_changed_from_timer_(false),
  prequeuing_(false),
  busy_(false),
  our_cache_background_task_(nullptr),
  autocache_(true)
{
  // Set up main layout
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setMargin(0);

  // Set up stacked widget to allow switching away from the viewer widget
  stack_ = new QStackedWidget();
  layout->addWidget(stack_);

  // Create main OpenGL-based view and sizer
  sizer_ = new ViewerSizer();
  stack_->addWidget(sizer_);

  display_widget_ = new ViewerDisplayWidget();
  connect(display_widget_, &ViewerDisplayWidget::customContextMenuRequested, this, &ViewerWidget::ShowContextMenu);
  connect(display_widget_, &ViewerDisplayWidget::CursorColor, this, &ViewerWidget::CursorColor);
  connect(display_widget_, &ViewerDisplayWidget::ColorProcessorChanged, this, &ViewerWidget::ColorProcessorChanged);
  connect(display_widget_, &ViewerDisplayWidget::ColorManagerChanged, this, &ViewerWidget::ColorManagerChanged);
  connect(sizer_, &ViewerSizer::RequestMatrix, display_widget_, &ViewerDisplayWidget::SetMatrix);
  sizer_->SetWidget(display_widget_);

  // Create waveform view when audio is connected and video isn't
  waveform_view_ = new AudioWaveformView();
  stack_->addWidget(waveform_view_);

  // Create time ruler
  layout->addWidget(ruler());

  // Create scrollbar
  layout->addWidget(scrollbar());
  connect(scrollbar(), &QScrollBar::valueChanged, ruler(), &TimeRuler::SetScroll);
  connect(scrollbar(), &QScrollBar::valueChanged, waveform_view_, &AudioWaveformView::SetScroll);

  // Create lower controls
  controls_ = new PlaybackControls();
  controls_->SetTimecodeEnabled(true);
  controls_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  connect(controls_, &PlaybackControls::PlayClicked, this, static_cast<void(ViewerWidget::*)()>(&ViewerWidget::Play));
  connect(controls_, &PlaybackControls::PauseClicked, this, &ViewerWidget::Pause);
  connect(controls_, &PlaybackControls::PrevFrameClicked, this, &ViewerWidget::PrevFrame);
  connect(controls_, &PlaybackControls::NextFrameClicked, this, &ViewerWidget::NextFrame);
  connect(controls_, &PlaybackControls::BeginClicked, this, &ViewerWidget::GoToStart);
  connect(controls_, &PlaybackControls::EndClicked, this, &ViewerWidget::GoToEnd);
  connect(controls_, &PlaybackControls::TimeChanged, this, &ViewerWidget::SetTimeAndSignal);
  layout->addWidget(controls_);

  // FIXME: Magic number
  SetScale(48.0);

  // Start background renderer
  renderer_ = new OpenGLBackend(this);
  renderer_->SetUpdateWithGraph(true);
  renderer_->SetRenderMode(RenderMode::kOffline);

  // Setup cache wait timer (waits a few seconds of inactivity before caching)
  cache_wait_timer_.setInterval(Config::Current()["AutoCacheInterval"].toInt());
  cache_wait_timer_.setSingleShot(true);
  connect(&cache_wait_timer_, &QTimer::timeout, this, &ViewerWidget::StartBackgroundCaching);

  // Remove pointer to cache task if it's removed from the task manager
  connect(TaskManager::instance(), &TaskManager::TaskRemoved, this, &ViewerWidget::BackgroundCacheFinished);

  // Ensures that seeking on the waveform view updates the time as expected
  connect(waveform_view_, &AudioWaveformView::TimeChanged, this, &ViewerWidget::SetTimeAndSignal);

  // Ensures renderer is updated if the global pixel format is changed
  connect(PixelFormat::instance(), &PixelFormat::FormatChanged, this, &ViewerWidget::UpdateRendererParameters);

  connect(&playback_backup_timer_, &QTimer::timeout, this, &ViewerWidget::PlaybackTimerUpdate);

  SetAutoMaxScrollBar(true);
}

ViewerWidget::~ViewerWidget()
{
  QList<ViewerWindow*> windows = windows_;

  foreach (ViewerWindow* window, windows) {
    delete window;
  }
}

void ViewerWidget::TimeChangedEvent(const int64_t &i)
{
  if (!time_changed_from_timer_) {
    Pause();
  }

  controls_->SetTime(i);
  waveform_view_->SetTime(i);

  if (GetConnectedNode() && last_time_ != i) {
    rational time_set = Timecode::timestamp_to_time(i, timebase());

    if (!IsPlaying()) {
      UpdateTextureFromNode(time_set);

      PushScrubbedAudio();
    }

    display_widget_->SetTime(time_set);
  }

  last_time_ = i;
}

void ViewerWidget::ConnectNodeInternal(ViewerOutput *n)
{
  connect(n, &ViewerOutput::SizeChanged, this, &ViewerWidget::SizeChangedSlot);
  connect(n, &ViewerOutput::LengthChanged, this, &ViewerWidget::LengthChangedSlot);
  connect(n, &ViewerOutput::ParamsChanged, this, &ViewerWidget::UpdateRendererParameters);
  connect(n->video_frame_cache(), &FrameHashCache::Invalidated, this, &ViewerWidget::ViewerInvalidatedVideoRange);
  connect(n->video_frame_cache(), &FrameHashCache::Shifted, this, &ViewerWidget::ViewerShiftedRange);
  connect(n->audio_playback_cache(), &FrameHashCache::Invalidated, this, &ViewerWidget::ViewerInvalidatedRange);
  connect(n, &ViewerOutput::GraphChangedFrom, this, &ViewerWidget::UpdateStack);

  ruler()->SetPlaybackCache(n->video_frame_cache());

  n->audio_playback_cache()->SetParameters(n->audio_params());

  SizeChangedSlot(n->video_params().width(), n->video_params().height());
  last_length_ = rational();
  LengthChangedSlot(n->GetLength());

  ColorManager* using_manager;
  if (override_color_manager_) {
    using_manager = override_color_manager_;
  } else if (n->parent()) {
    using_manager = static_cast<Sequence*>(n->parent())->project()->color_manager();
  } else {
    qWarning() << "Failed to find a suitable color manager for the connected viewer node";
    using_manager = nullptr;
  }

  display_widget_->ConnectColorManager(using_manager);
  foreach (ViewerWindow* window, windows_) {
    window->display_widget()->ConnectColorManager(using_manager);
  }

  UpdateStack();

  if (GetConnectedTimelinePoints()) {
    waveform_view_->SetViewer(GetConnectedNode()->audio_playback_cache());
    waveform_view_->ConnectTimelinePoints(GetConnectedTimelinePoints());
  }

  UpdateRendererParameters();

  // Set texture to new texture (or null if no viewer node is available)
  ForceUpdate();
}

void ViewerWidget::DisconnectNodeInternal(ViewerOutput *n)
{
  PauseInternal();

  if (cache_background_task_ == our_cache_background_task_) {
    StopAllBackgroundCacheTasks(true);
    cache_background_task_ = nullptr;
  }
  cache_wait_timer_.stop();

  disconnect(n, &ViewerOutput::SizeChanged, this, &ViewerWidget::SizeChangedSlot);
  disconnect(n, &ViewerOutput::LengthChanged, this, &ViewerWidget::LengthChangedSlot);
  disconnect(n, &ViewerOutput::ParamsChanged, this, &ViewerWidget::UpdateRendererParameters);
  disconnect(n->video_frame_cache(), &FrameHashCache::Invalidated, this, &ViewerWidget::ViewerInvalidatedVideoRange);
  disconnect(n->video_frame_cache(), &FrameHashCache::Shifted, this, &ViewerWidget::ViewerShiftedRange);
  disconnect(n->audio_playback_cache(), &FrameHashCache::Invalidated, this, &ViewerWidget::ViewerInvalidatedRange);
  disconnect(n, &ViewerOutput::GraphChangedFrom, this, &ViewerWidget::UpdateStack);

  ruler()->SetPlaybackCache(nullptr);

  // Effectively disables the viewer and clears the state
  SizeChangedSlot(0, 0);

  display_widget_->DisconnectColorManager();
  foreach (ViewerWindow* window, windows_) {
    window->display_widget()->DisconnectColorManager();
  }

  waveform_view_->SetViewer(nullptr);
  waveform_view_->ConnectTimelinePoints(nullptr);
}

void ViewerWidget::ConnectedNodeChanged(ViewerOutput *n)
{
  renderer_->SetViewerNode(n);
}

void ViewerWidget::ScaleChangedEvent(const double &s)
{
  TimeBasedWidget::ScaleChangedEvent(s);

  waveform_view_->SetScale(s);
}

void ViewerWidget::resizeEvent(QResizeEvent *event)
{
  TimeBasedWidget::resizeEvent(event);

  /*
  int new_div = CalculateDivider();
  if (new_div != divider_) {
    divider_ = new_div;

    UpdateRendererParameters();
  }
  */

  UpdateMinimumScale();
}

ViewerDisplayWidget *ViewerWidget::display_widget() const
{
  return display_widget_;
}

void ViewerWidget::TogglePlayPause()
{
  if (IsPlaying()) {
    Pause();
  } else {
    Play();
  }
}

bool ViewerWidget::IsPlaying() const
{
  return playback_speed_ != 0;
}

void ViewerWidget::ConnectViewerNode(ViewerOutput *node, ColorManager* color_manager)
{
  override_color_manager_ = color_manager;

  TimeBasedWidget::ConnectViewerNode(node);
}

void ViewerWidget::SetColorMenuEnabled(bool enabled)
{
  color_menu_enabled_ = enabled;
}

void ViewerWidget::SetOverrideSize(int width, int height)
{
  SizeChangedSlot(width, height);
}

void ViewerWidget::SetMatrix(const QMatrix4x4 &mat)
{
  display_widget_->SetMatrix(mat);
  foreach (ViewerWindow* vw, windows_) {
    vw->display_widget()->SetMatrix(mat);
  }
}

void ViewerWidget::SetFullScreen(QScreen *screen)
{
  if (!screen) {
    // Try to find the screen that contains the mouse cursor currently
    foreach (QScreen* test, QGuiApplication::screens()) {
      if (test->geometry().contains(QCursor::pos())) {
        screen = test;
        break;
      }
    }

    // Fallback, just use the first screen
    if (!screen) {
      screen = QGuiApplication::screens().first();
    }
  }

  ViewerWindow* vw = new ViewerWindow(this);

  vw->setGeometry(screen->geometry());
  vw->showFullScreen();
  vw->display_widget()->ConnectColorManager(color_manager());
  connect(vw, &ViewerWindow::destroyed, this, &ViewerWidget::WindowAboutToClose);
  connect(vw->display_widget(), &ViewerDisplayWidget::customContextMenuRequested, this, &ViewerWidget::ShowContextMenu);

  if (GetConnectedNode()) {
    vw->SetResolution(GetConnectedNode()->video_params().width(), GetConnectedNode()->video_params().height());
  }

  vw->display_widget()->SetImage(display_widget_->last_loaded_buffer());

  windows_.append(vw);
}

void ViewerWidget::ForceUpdate()
{
  // Hack that forces the viewer to update
  UpdateTextureFromNode(GetTime());
}

void ViewerWidget::SetAutoCacheEnabled(bool e)
{
  autocache_ = e;

  if (autocache_) {
    StartBackgroundCaching();
  } else if (cache_background_task_ == our_cache_background_task_) {
    StopAllBackgroundCacheTasks(false);
    cache_background_task_ = nullptr;
  }
}

void ViewerWidget::SetGizmos(Node *node)
{
  display_widget_->SetTimeTarget(GetConnectedNode());
  display_widget_->SetGizmos(node);
}

void ViewerWidget::StopAllBackgroundCacheTasks(bool wait)
{
  if (cache_background_task_) {
    if (wait) {
      TaskManager::instance()->CancelTaskAndWait(cache_background_task_);
    } else {
      cache_background_task_->Cancel();
    }
    cache_background_task_ = nullptr;
  }
}

void ViewerWidget::SetBackgroundCacheTask(CacheTask *t)
{
  cache_background_task_ = t;
}

FramePtr DecodeCachedImage(const QString &fn, const rational& time)
{
  FramePtr frame = FrameHashCache::LoadCacheFrame(fn);

  if (frame) {
    frame->set_timestamp(time);
  } else {
    qWarning() << "Tried to load cached frame from file but it was null";
  }

  return frame;
}

void DecodeCachedImage(RenderTicketPtr ticket, const QString &fn, const rational& time)
{
  ticket->Finish(QVariant::fromValue(DecodeCachedImage(fn, time)));
}

void ViewerWidget::UpdateTextureFromNode(const rational& time)
{
  bool frame_exists_at_time = FrameExistsAtTime(time);

  // Check playback queue for a frame
  if (IsPlaying()) {

    // We still run the playback queue even when FrameExistsAtTime returns false because we might be
    // playing backwards and about to start showing frames, so the queue should be prepared for
    // that.
    while (!playback_queue_.empty()) {

      const ViewerPlaybackFrame& pf = playback_queue_.front();

      if (pf.timestamp == time) {

        // Frame was in queue, no need to decode anything
        SetDisplayImage(pf.frame, true);
        return;

      } else {

        // Skip this frame
        PopOldestFrameFromPlaybackQueue();

      }
    }

    // Only show warning if frame actually exists
    if (frame_exists_at_time) {
      qWarning() << "Playback queue failed to keep up";
    }

  }

  if (!frame_exists_at_time) {
    // There is definitely no frame here, we can immediately flip to showing nothing
    nonqueue_watchers_.clear();
    SetDisplayImage(nullptr, false);
    return;
  }

  // Frame was not in queue, will require rendering or decoding from cache
  RenderTicketWatcher* watcher = new RenderTicketWatcher();
  connect(watcher, &RenderTicketWatcher::Finished, this, &ViewerWidget::RendererGeneratedFrame);
  nonqueue_watchers_.append(watcher);
  watcher->SetTicket(GetFrame(time, true));
}

void ViewerWidget::PlayInternal(int speed, bool in_to_out_only)
{
  Q_ASSERT(speed != 0);

  if (timebase().isNull()) {
    qWarning() << "ViewerWidget can't play with an invalid timebase";
    return;
  }

  // If the playhead is beyond the end, restart at 0
  if (!in_to_out_only && GetTime() >= GetConnectedNode()->GetLength()) {
    if (speed > 0) {
      SetTimeAndSignal(0);
    } else {
      SetTimeAndSignal(Timecode::time_to_timestamp(GetConnectedNode()->GetLength(), timebase()));
    }
  }

  playback_speed_ = speed;
  play_in_to_out_only_ = in_to_out_only;

  playback_queue_next_frame_ = ruler()->GetTime();

  controls_->ShowPauseButton();

  // Attempt to fill playback queue
  if (stack_->currentWidget() == sizer_) {
    prequeue_length_ = DeterminePlaybackQueueSize();

    if (prequeue_length_ > 0) {
      prequeuing_ = true;

      for (int i=0; i<prequeue_length_; i++) {
        RequestNextFrameForQueue();
      }
    }
  }

  if (!busy_) {
    busy_ = true;
    busy_viewers_++;
  }

  StopAllBackgroundCacheTasks(false);
  cache_wait_timer_.stop();

  if (!prequeuing_) {
    FinishPlayPreprocess();
  }
}

void ViewerWidget::PauseInternal()
{
  if (IsPlaying()) {
    AudioManager::instance()->StopOutput();
    playback_speed_ = 0;
    controls_->ShowPlayButton();

    disconnect(display_widget_, &ViewerDisplayWidget::frameSwapped, this, &ViewerWidget::ForceUpdate);

    foreach (ViewerWindow* window, windows_) {
      window->Pause();
    }

    playback_queue_.clear();
    playback_backup_timer_.stop();
  }

  prequeuing_ = false;
}

void ViewerWidget::PushScrubbedAudio()
{
  if (!IsPlaying() && Config::Current()["AudioScrubbing"].toBool()) {
    // Get audio src device from renderer
    QString audio_fn = GetConnectedNode()->audio_playback_cache()->GetCacheFilename();
    QFile audio_src(audio_fn);

    if (audio_src.open(QFile::ReadOnly)) {
      const AudioParams& params = GetConnectedNode()->audio_playback_cache()->GetParameters();

      // FIXME: Hardcoded scrubbing interval (20ms)
      int size_of_sample = params.time_to_bytes(rational(20, 1000));

      // Push audio
      audio_src.seek(params.time_to_bytes(GetTime()));
      QByteArray frame_audio = audio_src.read(size_of_sample);
      AudioManager::instance()->SetOutputParams(params);
      AudioManager::instance()->PushToOutput(frame_audio);

      audio_src.close();
    }
  }
}

/*
int ViewerWidget::CalculateDivider()
{
  if (GetConnectedNode() && Config::Current()["AutoSelectDivider"].toBool()) {
    int long_side_of_video = qMax(GetConnectedNode()->video_params().width(), GetConnectedNode()->video_params().height());
    int long_side_of_widget = qMax(display_widget_->width(), display_widget_->height());

    return qMax(1, int(qPow(2, qFloor(log2(double(long_side_of_video) / double(long_side_of_widget))))));
  }

  return divider_;
}
*/

void ViewerWidget::UpdateMinimumScale()
{
  if (!GetConnectedNode()) {
    return;
  }

  if (GetConnectedNode()->GetLength().isNull()) {
    // Avoids divide by zero
    SetMinimumScale(0);
  } else {
    SetMinimumScale(static_cast<double>(ruler()->width()) / GetConnectedNode()->GetLength().toDouble());
  }
}

void ViewerWidget::SetColorTransform(const ColorTransform &transform, ViewerDisplayWidget *sender)
{
  sender->SetColorTransform(transform);
}

QString ViewerWidget::GetCachedFilenameFromTime(const rational &time)
{
  if (FrameExistsAtTime(time)) {
    QByteArray hash = GetConnectedNode()->video_frame_cache()->GetHash(time);

    if (!hash.isEmpty()) {
      return GetConnectedNode()->video_frame_cache()->CachePathName(hash);
    }
  }

  return QString();
}

bool ViewerWidget::FrameExistsAtTime(const rational &time)
{
  return GetConnectedNode() && time >= 0 && time < GetConnectedNode()->video_frame_cache()->GetLength();
}

void ViewerWidget::SetDisplayImage(FramePtr frame, bool main_only)
{
  display_widget_->SetImage(frame);

  if (!main_only) {
    foreach (ViewerWindow* vw, windows_) {
      vw->display_widget()->SetImage(frame);
    }
  }

  emit LoadedBuffer(frame.get());
}

void ViewerWidget::RequestNextFrameForQueue()
{
  rational next_time = Timecode::timestamp_to_time(playback_queue_next_frame_,
                                                   timebase());

  playback_queue_next_frame_ += playback_speed_;

  RenderTicketWatcher* watcher = new RenderTicketWatcher();
  connect(watcher, &RenderTicketWatcher::Finished, this, &ViewerWidget::RendererGeneratedFrameForQueue);
  watcher->SetTicket(GetFrame(next_time, false));
}

PixelFormat::Format ViewerWidget::GetCurrentPixelFormat() const
{
  return PixelFormat::instance()->GetConfiguredFormatForMode(RenderMode::kOffline);
}

RenderTicketPtr ViewerWidget::GetFrame(const rational &t, bool clear_render_queue)
{
  QByteArray cached_hash = GetConnectedNode()->video_frame_cache()->GetHash(t);
  if (cached_hash.isEmpty()) {
    // Frame hasn't been cached, start render job
    if (clear_render_queue) {
      renderer_->ClearVideoQueue();
    }

    return renderer_->RenderFrame(t);
  } else {
    // Frame has been cached, grab the frame
    QString cache_fn = GetConnectedNode()->video_frame_cache()->CachePathName(cached_hash);

    RenderTicketPtr ticket = std::make_shared<RenderTicket>(RenderTicket::kTypeVideo,
                                                            QVariant::fromValue(t));
    QtConcurrent::run(DecodeCachedImage, ticket, cache_fn, t);
    return ticket;

  }
}

void ViewerWidget::FinishPlayPreprocess()
{
  int64_t playback_start_time = ruler()->GetTime();

  QString audio_fn = GetConnectedNode()->audio_playback_cache()->GetCacheFilename();
  if (!audio_fn.isEmpty()) {
    AudioManager::instance()->SetOutputParams(GetConnectedNode()->audio_playback_cache()->GetParameters());
    AudioManager::instance()->StartOutput(audio_fn,
                                          GetConnectedNode()->audio_playback_cache()->GetParameters().time_to_bytes(GetTime()),
                                          playback_speed_);
  }

  playback_timer_.Start(playback_start_time, playback_speed_, timebase_dbl());

  foreach (ViewerWindow* window, windows_) {
    window->Play(playback_start_time, playback_speed_, timebase());
  }

  connect(display_widget_, &ViewerDisplayWidget::frameSwapped, this, &ViewerWidget::ForceUpdate);

  playback_backup_timer_.setInterval(qFloor(timebase_dbl()));
  playback_backup_timer_.start();

  PlaybackTimerUpdate();
}

int ViewerWidget::DeterminePlaybackQueueSize()
{
  int64_t end_ts;

  if (playback_speed_ > 0) {
    end_ts = Timecode::time_to_timestamp(GetConnectedNode()->video_frame_cache()->GetLength(),
                                         timebase());
  } else {
    end_ts = 0;
  }

  int remaining_frames = (end_ts - GetTimestamp()) * playback_speed_;

  return qMin(kMaxPreQueueSize, remaining_frames);
}

void ViewerWidget::PopOldestFrameFromPlaybackQueue()
{
  playback_queue_.pop_front();

  if (int(playback_queue_.size()) < DeterminePlaybackQueueSize()) {
    RequestNextFrameForQueue();
  }
}

void ViewerWidget::UpdateStack()
{
  rational new_tb;

  if (GetConnectedNode()
      && !GetConnectedNode()->texture_input()->is_connected()
      && GetConnectedNode()->samples_input()->is_connected()) {
    // If we have a node AND video is disconnected AND audio is connected, show waveform view
    stack_->setCurrentWidget(waveform_view_);
    new_tb = GetConnectedNode()->audio_params().time_base();
  } else {
    // Otherwise show regular display
    stack_->setCurrentWidget(sizer_);
    new_tb = GetConnectedNode()->video_params().time_base();
  }

  if (new_tb != timebase()) {
    SetTimebase(new_tb);
  }
}

void ViewerWidget::ContextMenuSetFullScreen(QAction *action)
{
  SetFullScreen(QGuiApplication::screens().at(action->data().toInt()));
}

void ViewerWidget::ContextMenuDisableSafeMargins()
{
  context_menu_widget_->SetSafeMargins(ViewerSafeMarginInfo(false));
}

void ViewerWidget::ContextMenuSetSafeMargins()
{
  context_menu_widget_->SetSafeMargins(ViewerSafeMarginInfo(true));
}

void ViewerWidget::ContextMenuSetCustomSafeMargins()
{
  bool ok;

  double new_ratio = GetFloatRatioFromUser(this,
                                           tr("Safe Margins"),
                                           &ok);

  if (ok) {
    context_menu_widget_->SetSafeMargins(ViewerSafeMarginInfo(true, new_ratio));
  }
}

void ViewerWidget::WindowAboutToClose()
{
  windows_.removeOne(static_cast<ViewerWindow*>(sender()));
}

void ViewerWidget::ContextMenuScopeTriggered(QAction *action)
{
  emit RequestScopePanel(static_cast<ScopePanel::Type>(action->data().toInt()));
}

void ViewerWidget::RendererGeneratedFrame()
{
  RenderTicketWatcher* ticket = static_cast<RenderTicketWatcher*>(sender());

  if (!ticket->WasCancelled()) {
    FramePtr frame = ticket->Get().value<FramePtr>();

    if (nonqueue_watchers_.contains(ticket)) {
      while (!nonqueue_watchers_.isEmpty()) {
        if (nonqueue_watchers_.takeFirst() == ticket) {
          break;
        }
      }

      SetDisplayImage(frame, false);
    }
  }

  ticket->deleteLater();
}

void ViewerWidget::RendererGeneratedFrameForQueue()
{
  RenderTicketWatcher* watcher = static_cast<RenderTicketWatcher*>(sender());

  if (!watcher->WasCancelled()) {
    FramePtr frame = watcher->Get().value<FramePtr>();

    // Ignore this signal if we've paused now
    if (IsPlaying() || prequeuing_) {
      playback_queue_.AppendTimewise({frame->timestamp(), frame}, playback_speed_);

      foreach (ViewerWindow* window, windows_) {
        window->queue()->AppendTimewise({frame->timestamp(), frame}, playback_speed_);
      }

      if (prequeuing_ && int(playback_queue_.size()) == prequeue_length_) {
        prequeuing_ = false;
        FinishPlayPreprocess();
      }
    }
  }

  watcher->deleteLater();
}

//#define PRINT_INVALID_RANGES
void ViewerWidget::StartBackgroundCaching()
{
  if (busy_) {
    busy_viewers_--;
    busy_ = false;
  }

#ifdef PRINT_INVALID_RANGES
  if (GetConnectedNode()->video_frame_cache()->HasInvalidatedRanges()) {
    qDebug() << "Video invalid:";
    foreach (const TimeRange& r, GetConnectedNode()->video_frame_cache()->GetInvalidatedRanges()) {
      qDebug() << "  " << r;
    }
  }

  if (GetConnectedNode()->audio_playback_cache()->HasInvalidatedRanges()) {
    qDebug() << "Audio invalid:";
    foreach (const TimeRange& r, GetConnectedNode()->audio_playback_cache()->GetInvalidatedRanges()) {
      qDebug() << "  " << r;
    }
  }
#endif

  if (autocache_
      && GetConnectedNode()
      && (GetConnectedNode()->video_frame_cache()->HasInvalidatedRanges()
          || GetConnectedNode()->audio_playback_cache()->HasInvalidatedRanges())) {
    if (cache_background_task_ || busy_viewers_) {

      // Something else is caching right now, we don't want to do multiple at once so we'll check
      // again in our next interval
      cache_wait_timer_.start();

    } else {
      cache_background_task_ = new CacheTask(renderer_, false);

      our_cache_background_task_ = cache_background_task_;

      TaskManager::instance()->AddTask(cache_background_task_);
    }
  }
}

void ViewerWidget::BackgroundCacheFinished(Task* t)
{
  if (cache_background_task_ == t) {
    cache_background_task_ = nullptr;
  }
}

void ViewerWidget::UpdateRendererParameters()
{
  if (cache_background_task_ == our_cache_background_task_) {
    StopAllBackgroundCacheTasks(false);
  }

  GetConnectedNode()->video_frame_cache()->InvalidateAll();
  GetConnectedNode()->audio_playback_cache()->InvalidateAll();

  renderer_->SetVideoParams(GetConnectedNode()->video_params());
  renderer_->SetAudioParams(GetConnectedNode()->audio_params());

  display_widget_->SetVideoParams(GetConnectedNode()->video_params());
}

void ViewerWidget::ShowContextMenu(const QPoint &pos)
{
  Menu menu(static_cast<QWidget*>(sender()));

  context_menu_widget_ = static_cast<ViewerDisplayWidget*>(sender());

  // Color options
  if (context_menu_widget_->color_manager() && color_menu_enabled_) {
    {
      Menu* ocio_display_menu = context_menu_widget_->GetDisplayMenu(&menu);
      menu.addMenu(ocio_display_menu);
    }

    {
      Menu* ocio_view_menu = context_menu_widget_->GetViewMenu(&menu);
      menu.addMenu(ocio_view_menu);
    }

    {
      Menu* ocio_look_menu = context_menu_widget_->GetLookMenu(&menu);
      menu.addMenu(ocio_look_menu);
    }

    menu.addSeparator();
  }

  {
    // Viewer Zoom Level
    Menu* zoom_menu = new Menu(tr("Zoom"), &menu);
    menu.addMenu(zoom_menu);

    int zoom_levels[] = {10, 25, 50, 75, 100, 150, 200, 400};
    zoom_menu->addAction(tr("Fit"))->setData(0);
    for (int i=0;i<8;i++) {
      zoom_menu->addAction(tr("%1%").arg(zoom_levels[i]))->setData(zoom_levels[i]);
    }

    connect(zoom_menu, &QMenu::triggered, this, &ViewerWidget::SetZoomFromMenu);
  }

  {
    // Full Screen Menu
    Menu* full_screen_menu = new Menu(tr("Full Screen"), &menu);
    menu.addMenu(full_screen_menu);

    for (int i=0;i<QGuiApplication::screens().size();i++) {
      QScreen* s = QGuiApplication::screens().at(i);

      QAction* a = full_screen_menu->addAction(tr("Screen %1: %2x%3").arg(QString::number(i),
                                                                          QString::number(s->size().width()),
                                                                          QString::number(s->size().height())));

      a->setData(i);
    }

    connect(full_screen_menu, &QMenu::triggered, this, &ViewerWidget::ContextMenuSetFullScreen);
  }

  menu.addSeparator();

  {
    // Scopes
    Menu* scopes_menu = new Menu(tr("Scopes"), &menu);
    menu.addMenu(scopes_menu);

    for (int i=0;i<ScopePanel::kTypeCount;i++) {
      QAction* scope_action = scopes_menu->addAction(ScopePanel::TypeToName(static_cast<ScopePanel::Type>(i)));
      scope_action->setData(i);
    }

    connect(scopes_menu, &Menu::triggered, this, &ViewerWidget::ContextMenuScopeTriggered);
  }

  menu.addSeparator();

  {
    // Auto-cache
    QAction* autocache_action = menu.addAction(tr("Auto-Cache"));
    autocache_action->setCheckable(true);
    autocache_action->setChecked(autocache_);
    connect(autocache_action, &QAction::triggered, this, &ViewerWidget::SetAutoCacheEnabled);
  }

  menu.addSeparator();

  {
    // Safe Margins
    Menu* safe_margin_menu = new Menu(tr("Safe Margins"), &menu);
    menu.addMenu(safe_margin_menu);

    QAction* safe_margin_off = safe_margin_menu->addAction(tr("Off"));
    safe_margin_off->setCheckable(true);
    safe_margin_off->setChecked(!context_menu_widget_->GetSafeMargin().is_enabled());
    connect(safe_margin_off, &QAction::triggered, this, &ViewerWidget::ContextMenuDisableSafeMargins);

    QAction* safe_margin_on = safe_margin_menu->addAction(tr("On"));
    safe_margin_on->setCheckable(true);
    safe_margin_on->setChecked(context_menu_widget_->GetSafeMargin().is_enabled() && !context_menu_widget_->GetSafeMargin().custom_ratio());
    connect(safe_margin_on, &QAction::triggered, this, &ViewerWidget::ContextMenuSetSafeMargins);

    QAction* safe_margin_custom = safe_margin_menu->addAction(tr("Custom Aspect"));
    safe_margin_custom->setCheckable(true);
    safe_margin_custom->setChecked(context_menu_widget_->GetSafeMargin().is_enabled() && context_menu_widget_->GetSafeMargin().custom_ratio());
    connect(safe_margin_custom, &QAction::triggered, this, &ViewerWidget::ContextMenuSetCustomSafeMargins);
  }

  menu.exec(static_cast<QWidget*>(sender())->mapToGlobal(pos));
}

void ViewerWidget::Play(bool in_to_out_only)
{
  if (in_to_out_only) {
    if (GetConnectedTimelinePoints()
      && GetConnectedTimelinePoints()->workarea()->enabled()) {
      // Jump to in point
      SetTimeAndSignal(Timecode::time_to_timestamp(GetConnectedTimelinePoints()->workarea()->in(), timebase()));
    } else {
      in_to_out_only = false;
    }
  }

  PlayInternal(1, in_to_out_only);
}

void ViewerWidget::Play()
{
  Play(false);
}

void ViewerWidget::Pause()
{
  PauseInternal();

  StartBackgroundCaching();
}

void ViewerWidget::ShuttleLeft()
{
  int current_speed = playback_speed_;

  if (current_speed != 0) {
    PauseInternal();
  }

  current_speed--;

  if (current_speed == 0) {
    current_speed--;
  }

  PlayInternal(current_speed, false);
}

void ViewerWidget::ShuttleStop()
{
  Pause();
}

void ViewerWidget::ShuttleRight()
{
  int current_speed = playback_speed_;

  if (current_speed != 0) {
    PauseInternal();
  }

  current_speed++;

  if (current_speed == 0) {
    current_speed++;
  }

  PlayInternal(current_speed, false);
}

void ViewerWidget::SetColorTransform(const ColorTransform &transform)
{
  SetColorTransform(transform, display_widget_);
}

void ViewerWidget::SetSignalCursorColorEnabled(bool e)
{
  display_widget_->SetSignalCursorColorEnabled(e);

  foreach (ViewerWindow* vw, windows_) {
    vw->display_widget()->SetSignalCursorColorEnabled(e);
  }
}

void ViewerWidget::TimebaseChangedEvent(const rational &timebase)
{
  TimeBasedWidget::TimebaseChangedEvent(timebase);

  controls_->SetTimebase(timebase);

  controls_->SetTime(ruler()->GetTime());
  LengthChangedSlot(GetConnectedNode() ? GetConnectedNode()->GetLength() : 0);
}

void ViewerWidget::PlaybackTimerUpdate()
{
  int64_t current_time = playback_timer_.GetTimestampNow();

  int64_t min_time, max_time;

  {
    if ((play_in_to_out_only_ || Config::Current()["Loop"].toBool())
        && GetConnectedTimelinePoints()
        && GetConnectedTimelinePoints()->workarea()->enabled()) {

      // If "play in to out" is enabled or we're looping AND we have a workarea, only play the workarea
      min_time = Timecode::time_to_timestamp(GetConnectedTimelinePoints()->workarea()->in(), timebase());
      max_time = Timecode::time_to_timestamp(GetConnectedTimelinePoints()->workarea()->out(), timebase());

    } else {

      // Otherwise set the bounds to the range of the sequence
      min_time = 0;
      max_time = Timecode::time_to_timestamp(GetConnectedNode()->GetLength(), timebase());

    }
  }

  if ((playback_speed_ < 0 && current_time <= min_time)
      || (playback_speed_ > 0 && current_time >= max_time)) {

    // Determine which timestamp we tripped
    int64_t tripped_time;

    if (current_time <= min_time) {
      tripped_time = min_time;
    } else {
      tripped_time = max_time;
    }

    if (Config::Current()[QStringLiteral("Loop")].toBool()) {

      // If we're looping, jump to the other side of the workarea and continue
      int64_t opposing_time = (tripped_time == min_time) ? max_time : min_time;

      // Cache the current speed
      int current_speed = playback_speed_;

      // Jump to the other side and keep playing at the same speed
      SetTimeAndSignal(opposing_time);
      PlayInternal(current_speed, play_in_to_out_only_);

    } else {

      // Pause at the boundary
      SetTimeAndSignal(tripped_time);

    }

  } else {

    // Sets time, wrapping in this bool ensures we don't pause from setting the time
    time_changed_from_timer_ = true;
    SetTimeAndSignal(current_time);
    time_changed_from_timer_ = false;

  }

  if (!isVisible()) {
    while (!playback_queue_.empty() && playback_queue_.front().timestamp != GetTime()) {
      PopOldestFrameFromPlaybackQueue();
    }
  }
}

void ViewerWidget::SizeChangedSlot(int width, int height)
{
  sizer_->SetChildSize(width, height);

  foreach (ViewerWindow* vw, windows_) {
    vw->SetResolution(width, height);
  }
}

void ViewerWidget::LengthChangedSlot(const rational &length)
{
  if (last_length_ != length) {
    controls_->SetEndTime(Timecode::time_to_timestamp(length, timebase()));
    UpdateMinimumScale();

    if (length < last_length_ && GetTime() >= length) {
      ForceUpdate();
    }

    last_length_ = length;
  }
}

void ViewerWidget::SetZoomFromMenu(QAction *action)
{
  sizer_->SetZoom(action->data().toInt());
}

void ViewerWidget::ViewerInvalidatedVideoRange(const TimeRange &range)
{
  if (GetTime() >= range.in() && (GetTime() < range.out() || range.in() == range.out())) {
    QMetaObject::invokeMethod(this, "ForceUpdate", Qt::QueuedConnection);
  }

  ViewerInvalidatedRange();
}

void ViewerWidget::ViewerInvalidatedRange()
{
  // Restart the cache wait timer
  cache_wait_timer_.stop();
  StopAllBackgroundCacheTasks(false);

  if (!(qApp->mouseButtons() & Qt::LeftButton)) {
    cache_wait_timer_.start();
  }
}

void ViewerWidget::ViewerShiftedRange(const rational &from, const rational &to)
{
  if (GetTime() >= qMin(from, to)) {
    QMetaObject::invokeMethod(this, "ForceUpdate", Qt::QueuedConnection);
  }
}

OLIVE_NAMESPACE_EXIT
