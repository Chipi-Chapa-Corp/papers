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

// A small multi-page PDF keeps transfer tests independent of optional fixtures.
pub fn multipage_fixture() -> gio::File {
    use std::fmt::Write;
    let (file, stream) = gio::File::new_tmp(Some("papers-tab-pages-XXXXXX.pdf")).unwrap();
    stream.close(gio::Cancellable::NONE).unwrap();
    let mut pdf = String::from("%PDF-1.4\n");
    let mut offsets = vec![0];
    for object in [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >>",
    ] {
        offsets.push(pdf.len());
        writeln!(pdf, "{} 0 obj\n{object}\nendobj", offsets.len() - 1).unwrap();
    }
    let xref = pdf.len();
    pdf.push_str("xref\n0 6\n0000000000 65535 f \n");
    for offset in &offsets[1..] {
        writeln!(pdf, "{offset:010} 00000 n ").unwrap();
    }
    writeln!(
        pdf,
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF"
    )
    .unwrap();
    std::fs::write(file.path().unwrap(), pdf).unwrap();
    file
}
