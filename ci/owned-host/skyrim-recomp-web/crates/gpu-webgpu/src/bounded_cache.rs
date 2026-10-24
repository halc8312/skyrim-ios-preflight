//! Fixed-entry cache with constant-time replacement and least-recent eviction.
//! Removing a cache reference never destroys a resource held by an encoded draw.

use std::collections::HashMap;
use std::hash::{BuildHasher, Hash, RandomState};

struct Entry<K, V> {
    key: K,
    value: V,
    older: Option<usize>,
    newer: Option<usize>,
}

pub struct BoundedCache<K, V, S = RandomState> {
    index: HashMap<K, usize, S>,
    entries: Vec<Option<Entry<K, V>>>,
    free: Vec<usize>,
    oldest: Option<usize>,
    newest: Option<usize>,
    capacity: usize,
    evictions: u64,
    replacements: u64,
}

impl<K: Clone + Eq + Hash, V> BoundedCache<K, V> {
    pub fn new(capacity: usize) -> Self {
        Self::with_hasher(capacity, RandomState::new())
    }
}

impl<K: Clone + Eq + Hash, V, S: BuildHasher> BoundedCache<K, V, S> {
    pub fn with_hasher(capacity: usize, hasher: S) -> Self {
        assert!(capacity > 0);
        Self {
            index: HashMap::with_capacity_and_hasher(capacity, hasher),
            entries: Vec::new(), free: Vec::new(), oldest: None, newest: None,
            capacity, evictions: 0, replacements: 0,
        }
    }

    pub fn len(&self) -> usize { self.index.len() }
    pub fn is_empty(&self) -> bool { self.index.is_empty() }
    pub fn evictions(&self) -> u64 { self.evictions }
    pub fn replacements(&self) -> u64 { self.replacements }

    fn unlink(&mut self, slot: usize) {
        let entry = self.entries[slot].as_ref().unwrap();
        let (older, newer) = (entry.older, entry.newer);
        match older {
            Some(i) => self.entries[i].as_mut().unwrap().newer = newer,
            None => self.oldest = newer,
        }
        match newer {
            Some(i) => self.entries[i].as_mut().unwrap().older = older,
            None => self.newest = older,
        }
    }

    fn link_newest(&mut self, slot: usize) {
        let entry = self.entries[slot].as_mut().unwrap();
        entry.older = self.newest;
        entry.newer = None;
        if let Some(i) = self.newest {
            self.entries[i].as_mut().unwrap().newer = Some(slot);
        } else {
            self.oldest = Some(slot);
        }
        self.newest = Some(slot);
    }

    fn touch(&mut self, slot: usize) {
        if self.newest != Some(slot) {
            self.unlink(slot);
            self.link_newest(slot);
        }
    }

    pub fn get(&mut self, key: &K) -> Option<&V> {
        self.get_mut(key).map(|value| &*value)
    }

    pub fn get_mut(&mut self, key: &K) -> Option<&mut V> {
        let slot = *self.index.get(key)?;
        self.touch(slot);
        Some(&mut self.entries[slot].as_mut().unwrap().value)
    }

    fn remove_slot(&mut self, slot: usize) -> V {
        self.unlink(slot);
        let entry = self.entries[slot].take().unwrap();
        self.index.remove(&entry.key);
        self.free.push(slot);
        entry.value
    }

    /// Returns a replaced or evicted value, if any. Cardinality never exceeds capacity.
    pub fn insert(&mut self, key: K, value: V) -> Option<V> {
        if let Some(&slot) = self.index.get(&key) {
            self.touch(slot);
            self.replacements = self.replacements.saturating_add(1);
            return Some(std::mem::replace(&mut self.entries[slot].as_mut().unwrap().value, value));
        }
        let retired = if self.len() == self.capacity {
            self.evictions = self.evictions.saturating_add(1);
            Some(self.remove_slot(self.oldest.expect("full cache has an oldest entry")))
        } else { None };
        let slot = self.free.pop().unwrap_or_else(|| {
            self.entries.push(None);
            self.entries.len() - 1
        });
        self.index.insert(key.clone(), slot);
        self.entries[slot] = Some(Entry { key, value, older: None, newer: None });
        self.link_newest(slot);
        retired
    }

    pub fn retain(&mut self, mut keep: impl FnMut(&K, &mut V) -> bool) {
        for slot in 0..self.entries.len() {
            let remove = self.entries[slot].as_mut().is_some_and(|e| !keep(&e.key, &mut e.value));
            if remove { self.remove_slot(slot); }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn capacity_replacement_collision_and_retained_resources() {
        #[derive(Clone, Eq, PartialEq)]
        struct Key(u32);
        impl Hash for Key {
            fn hash<H: std::hash::Hasher>(&self, state: &mut H) { 0u8.hash(state); }
        }
        let mut c = BoundedCache::new(2);
        let held = std::rc::Rc::new(7);
        c.insert(Key(1), held.clone());
        c.insert(Key(2), std::rc::Rc::new(8));
        c.insert(Key(2), std::rc::Rc::new(9));
        assert_eq!(c.len(), 2);
        assert_eq!(c.evictions(), 0);
        assert_eq!(**c.get(&Key(1)).unwrap(), 7);
        c.insert(Key(3), std::rc::Rc::new(10));
        assert!(c.get(&Key(2)).is_none());
        assert_eq!(c.evictions(), 1);
        assert_eq!(c.replacements(), 1);
        c.retain(|_, _| false);
        assert!(c.is_empty());
        assert_eq!(*held, 7);
        assert_eq!(std::rc::Rc::strong_count(&held), 1);
    }

    #[test]
    fn reuse_and_recency_match_independent_reference_after_many_mutations() {
        let mut cache = BoundedCache::new(7);
        let mut reference = std::collections::VecDeque::<(u32, u32)>::new();
        let mut random = 91u32;
        for step in 0..5000 {
            random = random.wrapping_mul(1664525).wrapping_add(1013904223);
            let key = (random >> 16) % 17;
            if step % 23 == 0 {
                cache.retain(|k, _| k % 3 != 0);
                reference.retain(|(k, _)| k % 3 != 0);
            } else if step % 3 == 0 {
                let expected = reference.iter().position(|(k, _)| *k == key).map(|i| {
                    let entry = reference.remove(i).unwrap();
                    reference.push_back(entry);
                    entry.1
                });
                assert_eq!(cache.get(&key).copied(), expected);
            } else {
                if let Some(i) = reference.iter().position(|(k, _)| *k == key) {
                    reference.remove(i);
                } else if reference.len() == 7 { reference.pop_front(); }
                reference.push_back((key, step));
                cache.insert(key, step);
            }
            assert_eq!(cache.len(), reference.len());
            assert!(cache.entries.len() <= 7);
            let mut actual = Vec::new();
            let mut cursor = cache.oldest;
            while let Some(slot) = cursor {
                assert!(actual.len() < 7, "recency chain cycle");
                let e = cache.entries[slot].as_ref().unwrap();
                actual.push((e.key, e.value));
                cursor = e.newer;
            }
            assert_eq!(actual, reference.iter().copied().collect::<Vec<_>>());
        }
    }
}
