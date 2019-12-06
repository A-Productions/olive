#ifndef RENDERBACKEND_H
#define RENDERBACKEND_H

#include <QLinkedList>

#include "common/constructors.h"
#include "decodercache.h"
#include "node/graph.h"
#include "node/output/viewer/viewer.h"
#include "renderworker.h"

class RenderBackend : public QObject
{
  Q_OBJECT
public:
  RenderBackend(QObject* parent = nullptr);

  DISABLE_COPY_MOVE(RenderBackend)

  bool Init();

  void Close();

  const QString& GetError() const;

  void SetViewerNode(ViewerOutput* viewer_node);

  void SetCacheName(const QString& s);

  bool IsInitiated();

public slots:
  virtual void InvalidateCache(const rational &start_range, const rational &end_range);

  bool Compile();

  void Decompile();

protected:
  void RegenerateCacheID();

  virtual bool InitInternal() = 0;

  virtual void CloseInternal() = 0;

  virtual bool CompileInternal() = 0;

  virtual void DecompileInternal() = 0;

  rational SequenceLength();

  const QVector<QThread*>& threads();

  /**
   * @brief Internal function for generating the cache ID
   */
  virtual bool GenerateCacheIDInternal(QCryptographicHash& hash) = 0;

  virtual void CacheIDChangedEvent(const QString& id);

  void SetError(const QString& error);

  virtual void ConnectViewer(ViewerOutput* node);
  virtual void DisconnectViewer(ViewerOutput* node);

  /**
   * @brief Function called when there are frames in the queue to cache
   *
   * This function is NOT thread-safe and should only be called in the main thread.
   */
  void CacheNext();

  void InitWorkers();

  virtual NodeInput* GetDependentInput() = 0;

  virtual void ConnectWorkerToThis(RenderWorker* worker) = 0;

  ViewerOutput* viewer_node() const;

  bool ViewerIsConnected() const;

  const QString& cache_id() const;

  void QueueValueUpdate(const TimeRange& range);

  void UpdateNodeInputs();

  bool WorkerIsBusy(RenderWorker* worker) const;
  void SetWorkerBusyState(RenderWorker* worker, bool busy);

  QList<TimeRange> cache_queue_;

  QVector<RenderWorker*> processors_;

  bool compiled_;

private:
  bool AllProcessorsAreAvailable() const;

  /**
   * @brief Internal list of RenderProcessThreads
   */
  QVector<QThread*> threads_;

  /**
   * @brief Internal variable that contains whether the Renderer has started or not
   */
  bool started_;

  /**
   * @brief Internal reference to attached viewer node
   */
  ViewerOutput* viewer_node_;

  /**
   * @brief Internal reference to the copied viewer node we made in the compilation process
   */
  ViewerOutput* copied_viewer_node_;

  /**
   * @brief Error string that can be set in SetError() to handle failures
   */
  QString error_;

  QString cache_name_;
  qint64 cache_time_;
  QString cache_id_;

  QList<Node*> source_node_list_;
  NodeGraph copied_graph_;

  bool value_update_queued_;
  TimeRange value_update_range_;

  bool recompile_queued_;
  bool input_update_queued_;

  QVector<bool> processor_busy_state_;

private slots:
  void QueueRecompile();

};

#endif // RENDERBACKEND_H
