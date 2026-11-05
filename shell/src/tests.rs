use crate::deps::*;
use std::path::PathBuf;
use std::time::{Duration, Instant};

pub fn run(test: impl FnOnce() + Send + std::panic::UnwindSafe + 'static) {
    gtk::test_synced(move || {
        thread_local! {
            static APPLICATION: PpsApplication = {
                glib::set_prgname(Some("papers-tests"));
                let app = PpsApplication::new();
                app.register(gio::Cancellable::NONE).unwrap();
                app
            };
        }
        APPLICATION.with(|_| test());
    });
}

pub fn wait_until(description: &str, predicate: impl Fn() -> bool) {
    glib::MainContext::default().block_on(async {
        let deadline = Instant::now() + Duration::from_secs(5);
        while !predicate() {
            assert!(
                Instant::now() < deadline,
                "Timed out waiting for {description}"
            );
            glib::timeout_future(Duration::from_millis(10)).await;
        }
    });
}

pub fn fixture(name: &str) -> gio::File {
    gio::File::for_path(
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../test-data")
            .join(name),
    )
}

pub fn find_widget_where<T: IsA<gtk::Widget>>(
    widget: &impl IsA<gtk::Widget>,
    predicate: &impl Fn(&T) -> bool,
) -> Option<T> {
    let widget = widget.as_ref();
    if let Ok(found) = widget.clone().downcast::<T>()
        && predicate(&found)
    {
        return Some(found);
    }
    let mut child = widget.first_child();
    while let Some(widget) = child {
        if let Some(found) = find_widget_where::<T>(&widget, predicate) {
            return Some(found);
        }
        child = widget.next_sibling();
    }
    None
}
