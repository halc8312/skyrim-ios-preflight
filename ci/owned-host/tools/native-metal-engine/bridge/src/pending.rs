use crate::*;
/// Owner-thread completion. The closure is owned by the ticket, never by the
/// device, so dropping an unobserved ticket cannot create a device/reference cycle.
pub struct Pending<T> {
    value: RefCell<Option<T>>,
    poll: RefCell<Option<Box<dyn FnMut() -> Option<T>>>>,
    cancel: Option<Box<dyn FnOnce()>>,
}
impl<T> Pending<T> {
    pub(crate) fn ready(v: T) -> Self {
        Self {
            value: RefCell::new(Some(v)),
            poll: RefCell::new(None),
            cancel: None,
        }
    }
    pub(crate) fn polling(f: impl FnMut() -> Option<T> + 'static) -> Self {
        Self {
            value: RefCell::new(None),
            poll: RefCell::new(Some(Box::new(f))),
            cancel: None,
        }
    }
    pub(crate) fn on_drop(mut self, f: impl FnOnce() + 'static) -> Self {
        self.cancel = Some(Box::new(f));
        self
    }
    fn progress(&self) {
        let mut p = self.poll.borrow_mut();
        if let Some(f) = p.as_mut() {
            if let Some(v) = f() {
                *self.value.borrow_mut() = Some(v);
                *p = None;
            }
        }
    }
    pub fn is_ready(&self) -> bool {
        self.progress();
        self.value.borrow().is_some()
    }
    pub fn try_take(&self) -> Option<T> {
        self.progress();
        self.value.borrow_mut().take()
    }
}
impl<T> Drop for Pending<T> {
    fn drop(&mut self) {
        if let Some(cancel) = self.cancel.take() {
            cancel();
        }
    }
}
impl<T> std::fmt::Debug for Pending<T> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Pending")
            .field("complete", &self.poll.borrow().is_none())
            .finish()
    }
}
pub(crate) fn block<T>(pending: Pending<Result<T>>) -> Result<T> {
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        if let Some(r) = pending.try_take() {
            return r;
        }
        if Instant::now() >= deadline {
            return Err(Error::WorkDone("native completion deadline".into()));
        }
        std::thread::sleep(Duration::from_millis(1));
    }
}
