//! The native device, Rc resources, encoders and retirements never leave this
//! thread. Only owned CPU packets and verified CPU results cross the channel.
use super::*;
use std::sync::{
    Mutex,
    atomic::{AtomicBool, AtomicU64, Ordering},
    mpsc,
};
use std::thread::JoinHandle;

struct Work {
    sequence: u64,
    batch: Batch,
    reply: mpsc::SyncSender<Result<CompletedBatch>>,
}
struct Sender {
    channel: Option<mpsc::SyncSender<Work>>,
    next: u64,
}
pub enum OfferError {
    /// Nothing was admitted; retry precisely this owned batch after progress.
    Busy(Batch),
    Stopped(Batch),
}
#[derive(Debug)]
pub enum WaitError {
    /// Work remains owned by the worker. Keep this ticket and wait again;
    /// submitting another copy would duplicate the command stream.
    Pending,
    Closed,
    Failed(Error),
}
pub struct Ticket {
    sequence: u64,
    result: mpsc::Receiver<Result<CompletedBatch>>,
}
impl Ticket {
    pub fn sequence(&self) -> u64 {
        self.sequence
    }
    pub fn wait(&self, timeout: Duration) -> std::result::Result<CompletedBatch, WaitError> {
        self.result
            .recv_timeout(timeout)
            .map_err(|e| match e {
                mpsc::RecvTimeoutError::Timeout => WaitError::Pending,
                mpsc::RecvTimeoutError::Disconnected => WaitError::Closed,
            })?
            .map_err(WaitError::Failed)
    }
}
pub struct NativeWorker {
    sender: Mutex<Sender>,
    completed: Arc<AtomicU64>,
    stopping: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}
impl NativeWorker {
    pub fn open(
        config: Config,
        options: TargetOptions,
        constant_bytes: u64,
        commit: Arc<dyn CpuCommit>,
        execution_timeout: Duration,
    ) -> Result<Self> {
        if execution_timeout.is_zero() || execution_timeout > Duration::from_secs(60) {
            return Err(Error::Rejected("native worker execution timeout"));
        }
        let (channel, incoming) = mpsc::sync_channel::<Work>(4);
        let (ready, startup) = mpsc::sync_channel(1);
        let completed = Arc::new(AtomicU64::new(0));
        let stopping = Arc::new(AtomicBool::new(false));
        let stop = stopping.clone();
        let published = completed.clone();
        let thread = std::thread::Builder::new()
            .name("native-gpu-owner".into())
            .spawn(move || {
                // Creation itself happens on the final owner thread. No unsafe
                // Send implementation, raw native handle or foreign queue import.
                let owner = Gpu::<backend::Platform>::open_with_config(config)
                    .and_then(|gpu| NativeSession::new(gpu, options, constant_bytes));
                let mut owner = match owner {
                    Ok(owner) => {
                        let _ = ready.send(Ok(()));
                        owner
                    }
                    Err(error) => {
                        let _ = ready.send(Err(error));
                        return;
                    }
                };
                let mut failure: Option<Error> = None;
                while let Ok(work) = incoming.recv() {
                    let result = if stop.load(Ordering::Acquire) {
                        Err(Error::Rejected("native worker stopping"))
                    } else if let Some(error) = &failure {
                        Err(error.clone())
                    } else {
                        match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                            owner.execute(work.batch, &*commit, execution_timeout)
                        })) {
                            Ok(result) => result,
                            Err(_) => Err(Error::Rejected("native owner thread unwound")),
                        }
                    };
                    if result.is_ok() {
                        published.store(work.sequence, Ordering::Release);
                    } else {
                        failure = result.as_ref().err().cloned();
                    }
                    // Dropping a ticket does not cancel or replay GPU work. The
                    // worker still verifies completion and retains queue ordering.
                    let _ = work.reply.send(result);
                }
            })
            .map_err(|e| Error::Backend(format!("native owner thread: {e}")))?;
        match startup.recv() {
            Ok(Ok(())) => Ok(Self {
                sender: Mutex::new(Sender {
                    channel: Some(channel),
                    next: 1,
                }),
                completed,
                stopping,
                thread: Some(thread),
            }),
            result => {
                drop(channel);
                let _ = thread.join();
                Err(result
                    .ok()
                    .and_then(Result::err)
                    .unwrap_or(Error::Rejected("native owner startup closed")))
            }
        }
    }
    pub fn offer(&self, batch: Batch) -> std::result::Result<Ticket, OfferError> {
        let mut state = self.sender.lock().unwrap_or_else(|e| e.into_inner());
        if self.stopping.load(Ordering::Acquire) {
            return Err(OfferError::Stopped(batch));
        }
        let Some(channel) = &state.channel else {
            return Err(OfferError::Stopped(batch));
        };
        if state.next == u64::MAX {
            return Err(OfferError::Stopped(batch));
        }
        let (reply, result) = mpsc::sync_channel(1);
        let sequence = state.next;
        match channel.try_send(Work {
            sequence,
            batch,
            reply,
        }) {
            Ok(()) => {
                state.next += 1;
                Ok(Ticket { sequence, result })
            }
            Err(mpsc::TrySendError::Full(work)) => Err(OfferError::Busy(work.batch)),
            Err(mpsc::TrySendError::Disconnected(work)) => Err(OfferError::Stopped(work.batch)),
        }
    }
    /// Verified prefix of COMPLETE batches, including required CPU visibility.
    pub fn completed(&self) -> u64 {
        self.completed.load(Ordering::Acquire)
    }
    /// Nonblocking stop request: finish the already-running batch, cancel all
    /// not-yet-started batches, reject new offers. Does not interrupt a callback
    /// already running. Call before acquiring any lock needed by CpuCommit and
    /// await that active ticket without holding such locks before dropping.
    pub fn request_stop(&self) {
        self.stopping.store(true, Ordering::Release);
    }
}
impl Drop for NativeWorker {
    fn drop(&mut self) {
        self.request_stop();
        // With the only sender closed, queued work is CANCELLED, the running
        // batch finishes, and native destruction happens on its owner thread.
        // CpuCommit must not depend on the dropping thread holding its lock.
        self.sender
            .get_mut()
            .unwrap_or_else(|e| e.into_inner())
            .channel
            .take();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
