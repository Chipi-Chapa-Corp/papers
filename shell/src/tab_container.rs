use crate::deps::*;
use std::cell::{Cell, OnceCell};

mod imp {
    use super::*;

    // Keep the bar outside the pages so selecting a tab never interrupts its
    // pointer gestures. The slot reserves its position below the active header.
    #[derive(Default)]
    pub struct PpsTabContainer {
        pub(super) view: OnceCell<adw::TabView>,
        pub(super) bar: OnceCell<adw::TabBar>,
        pub(super) slot: gtk::Box,
        pub(super) host: RefCell<Option<adw::ToolbarView>>,
        pub(super) bar_exposed: Cell<bool>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for PpsTabContainer {
        const NAME: &'static str = "PpsTabContainer";
        type Type = super::PpsTabContainer;
        type ParentType = gtk::Widget;
    }

    impl ObjectImpl for PpsTabContainer {
        fn dispose(&self) {
            self.obj().set_host(None);
            for child in [
                self.view.get().unwrap().upcast_ref::<gtk::Widget>(),
                self.bar.get().unwrap().upcast_ref::<gtk::Widget>(),
            ] {
                if child.parent().is_some() {
                    child.unparent();
                }
            }
        }
    }

    impl WidgetImpl for PpsTabContainer {
        fn request_mode(&self) -> gtk::SizeRequestMode {
            self.view.get().unwrap().request_mode()
        }

        fn measure(&self, orientation: gtk::Orientation, for_size: i32) -> (i32, i32, i32, i32) {
            let height = self
                .bar
                .get()
                .unwrap()
                .measure(gtk::Orientation::Vertical, -1)
                .1;
            self.slot.set_height_request(height);
            self.view.get().unwrap().measure(orientation, for_size)
        }

        fn size_allocate(&self, width: i32, height: i32, baseline: i32) {
            self.view
                .get()
                .unwrap()
                .allocate(width, height, baseline, None);
            if self.host.borrow().is_some()
                && let Some(bounds) = self.slot.compute_bounds(self.obj().as_ref())
            {
                // A collapsed sidebar can cover the slot. Keep the strip under
                // that modal overlay, including its input-blocking surface.
                let picked = self.view.get().unwrap().pick(
                    f64::from(bounds.x() + bounds.width() / 2.0),
                    f64::from(bounds.y() + bounds.height() / 2.0),
                    gtk::PickFlags::DEFAULT,
                );
                let exposed = picked
                    .is_none_or(|widget| widget == self.slot || widget.is_ancestor(&self.slot));
                self.bar_exposed.set(exposed);
                self.bar.get().unwrap().set_sensitive(exposed);
                self.bar.get().unwrap().allocate(
                    bounds.width().round() as i32,
                    bounds.height().round() as i32,
                    -1,
                    Some(
                        gtk::gsk::Transform::new()
                            .translate(&graphene::Point::new(bounds.x(), bounds.y())),
                    ),
                );
            }
        }

        fn snapshot(&self, snapshot: &gtk::Snapshot) {
            self.obj()
                .snapshot_child(self.view.get().unwrap(), snapshot);
            if self.host.borrow().is_some() && self.bar_exposed.get() {
                self.obj().snapshot_child(self.bar.get().unwrap(), snapshot);
            }
        }
    }
}

glib::wrapper! {
    pub struct PpsTabContainer(ObjectSubclass<imp::PpsTabContainer>)
        @extends gtk::Widget,
        @implements gtk::Accessible, gtk::Buildable, gtk::ConstraintTarget;
}

impl PpsTabContainer {
    pub fn new(view: &adw::TabView, bar: &adw::TabBar) -> Self {
        let obj: Self = glib::Object::new();
        let imp = obj.imp();
        imp.view.set(view.clone()).unwrap();
        imp.bar.set(bar.clone()).unwrap();
        view.set_parent(&obj);
        bar.set_parent(&obj);
        obj
    }

    pub fn set_host(&self, host: Option<adw::ToolbarView>) {
        let imp = self.imp();
        if *imp.host.borrow() == host {
            return;
        }
        if let Some(previous) = imp.host.take() {
            previous.remove(&imp.slot);
        }
        if let Some(host) = &host {
            host.add_top_bar(&imp.slot);
        }
        imp.host.replace(host);
        self.queue_resize();
    }

    #[cfg(test)]
    pub fn slot(&self) -> gtk::Box {
        self.imp().slot.clone()
    }
}
