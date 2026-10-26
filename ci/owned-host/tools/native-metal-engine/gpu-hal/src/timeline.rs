//! Pure success-prefix logic; terminal and successful are different concepts.
use crate::backend::Completion;
use crate::{Error, Serial};
use std::collections::BTreeMap;

#[derive(Default)]
pub(crate) struct Timeline {
    issued: Serial,
    verified: Serial,
    terminal: BTreeMap<Serial, Completion>,
    failure: Option<(Serial, String)>,
}
impl Timeline {
    pub fn issue(&mut self) -> Result<Serial, Error> {
        self.healthy()?;
        self.issued = self
            .issued
            .checked_add(1)
            .ok_or(Error::Rejected("serial exhaustion"))?;
        Ok(self.issued)
    }
    pub fn observe(&mut self, serial: Serial, result: Completion) {
        assert!(serial > 0 && serial <= self.issued, "unknown completion");
        if serial <= self.verified {
            assert_eq!(
                result,
                Completion::Success,
                "contradictory terminal completion"
            );
            return;
        }
        if let Completion::Failure(message) = &result {
            self.fail(serial, message.clone());
        }
        if result != Completion::Pending {
            self.terminal.insert(serial, result);
        }
        while self.verified < self.issued {
            let next = self.verified + 1;
            if self.failure.as_ref().is_some_and(|(s, _)| next >= *s) {
                break;
            }
            if self.terminal.get(&next) != Some(&Completion::Success) {
                break;
            }
            self.terminal.remove(&next);
            self.verified = next;
        }
    }
    pub fn fail(&mut self, serial: Serial, message: String) {
        // Already verified success is immutable; later device/fence errors
        // affect the first still-unverified serial at the earliest.
        let serial = serial.max(self.verified.saturating_add(1));
        if self.failure.as_ref().is_none_or(|(s, _)| serial < *s) {
            self.failure = Some((serial, message));
        }
    }
    pub fn healthy(&self) -> Result<(), Error> {
        match &self.failure {
            Some((serial, detail)) => Err(Error::Failed {
                serial: *serial,
                detail: detail.clone(),
            }),
            None => Ok(()),
        }
    }
    pub fn status(&self, serial: Serial) -> Result<bool, Error> {
        if serial > self.issued {
            return Err(Error::Rejected("wait for an unissued serial"));
        }
        if serial <= self.verified {
            return Ok(true);
        }
        if let Some((failed, detail)) = &self.failure {
            if serial >= *failed {
                return Err(Error::Failed {
                    serial: *failed,
                    detail: detail.clone(),
                });
            }
        }
        Ok(false)
    }
    pub fn verified(&self) -> Serial {
        self.verified
    }
    pub fn issued(&self) -> Serial {
        self.issued
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn out_of_order_success_cannot_complete_a_hole_or_a_failed_prefix() {
        let mut t = Timeline::default();
        for _ in 0..4 {
            t.issue().unwrap();
        }
        t.observe(3, Completion::Success);
        assert_eq!(t.verified(), 0);
        t.observe(1, Completion::Success);
        assert_eq!(t.verified(), 1);
        t.observe(2, Completion::Failure("injected".into()));
        t.observe(4, Completion::Success);
        assert_eq!(t.verified(), 1);
        assert_eq!(t.status(1), Ok(true));
        assert!(t.status(2).is_err() && t.status(4).is_err() && t.issue().is_err());
    }
    #[test]
    fn successful_prefix_advances_only_when_every_serial_is_terminal() {
        let mut t = Timeline::default();
        for _ in 0..3 {
            t.issue().unwrap();
        }
        t.observe(3, Completion::Success);
        t.observe(2, Completion::Pending);
        t.observe(1, Completion::Success);
        assert_eq!(t.verified(), 1);
        t.observe(2, Completion::Success);
        assert_eq!(t.verified(), 3);
        assert!(t.status(4).is_err());
    }
    #[test]
    fn later_failure_does_not_prevent_an_earlier_pending_submission_from_completing() {
        let mut t = Timeline::default();
        t.issue().unwrap();
        t.issue().unwrap();
        t.observe(2, Completion::Failure("later command buffer".into()));
        assert_eq!(t.status(1), Ok(false));
        assert!(t.status(2).is_err());
        t.observe(1, Completion::Success);
        t.observe(1, Completion::Success); // completion callback/fence can lag
        assert_eq!(t.status(1), Ok(true));
        assert_eq!(t.verified(), 1);
        assert!(!t.terminal.contains_key(&1));
    }
    #[test]
    fn a_late_device_error_cannot_revoke_verified_success() {
        let mut t = Timeline::default();
        t.issue().unwrap();
        t.observe(1, Completion::Success);
        t.fail(1, "late fence failure".into());
        assert_eq!(t.status(1), Ok(true));
        assert!(matches!(t.healthy(), Err(Error::Failed { serial: 2, .. })));
    }
}
