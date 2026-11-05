use super::*;
use crate::tests::{fixture, wait_until};

#[test]
fn document_loading_and_passwords_belong_to_each_tab() {
    crate::tests::run(|| {
        let document = PpsDocumentTab::new();
        let encrypted = PpsDocumentTab::new();
        let contents = gtk::Box::new(gtk::Orientation::Horizontal, 0);
        contents.append(&document);
        contents.append(&encrypted);
        let window = adw::Window::builder().content(&contents).build();
        window.present();

        document.open(&fixture("utf16le-annot.pdf"), None, None);
        encrypted.open(&fixture("PasswordEncrypted.pdf"), None, None);
        wait_until("document and password prompt", || {
            document.document().is_some()
                && encrypted.imp().mode.get() == WindowRunMode::PasswordView
                && window.visible_dialog().is_some()
        });
        assert!(!document.loading());
        assert!(encrypted.loading());
        assert!(encrypted.document().is_none());
        assert_eq!(document.display_name(), "utf16le-annot.pdf");

        let dialog = window
            .visible_dialog()
            .and_downcast::<adw::AlertDialog>()
            .unwrap();
        let entry = dialog
            .extra_child()
            .unwrap()
            .first_child()
            .and_downcast::<gtk::PasswordEntry>()
            .unwrap();
        entry.set_text("password");
        dialog
            .default_widget()
            .and_downcast::<gtk::Button>()
            .unwrap()
            .emit_clicked();
        wait_until("unlocked document", || encrypted.document().is_some());
        assert!(!encrypted.loading());
        assert_ne!(document.document(), encrypted.document());

        let loaded = document.document();
        let model = document.imp().document_view.model();
        model.set_sizing_mode(SizingMode::Free);
        model.set_scale(1.75);
        contents.remove(&document);
        let other_window = adw::Window::builder().content(&document).build();
        other_window.present();
        assert_eq!(document.document(), loaded);
        assert_eq!(document.imp().document_view.model(), model);
        assert_eq!(model.scale(), 1.75);

        let broken = PpsDocumentTab::new();
        contents.append(&broken);
        let (invalid, stream) = gio::File::new_tmp(Some("papers-tab-test-XXXXXX")).unwrap();
        std::fs::write(invalid.path().unwrap(), b"invalid document").unwrap();
        stream.close(gio::Cancellable::NONE).unwrap();
        broken.open(&invalid, None, None);
        wait_until("load error", || {
            broken.imp().mode.get() == WindowRunMode::ErrorView
        });
        assert!(document.document().is_some());
        assert!(encrypted.document().is_some());
        other_window.destroy();
        window.destroy();
        invalid.delete(gio::Cancellable::NONE).unwrap();
    });
}

#[test]
fn a_document_copy_keeps_its_temporary_file_alive() {
    crate::tests::run(|| {
        let downloaded = papers_document::mkstemp_file("document.XXXXXX.pdf").unwrap();
        std::fs::copy(
            fixture("utf16le-annot.pdf").path().unwrap(),
            downloaded.path().unwrap(),
        )
        .unwrap();
        let source = PpsDocumentTab::new();
        let copy = PpsDocumentTab::new();
        let contents = gtk::Box::new(gtk::Orientation::Horizontal, 0);
        contents.append(&source);
        contents.append(&copy);
        let window = adw::Window::builder().content(&contents).build();
        window.present();
        source.open(&downloaded, None, None);
        source
            .imp()
            .local_file
            .replace(Some(Rc::new(TemporaryFile(downloaded.clone()))));
        wait_until("temporary document", || source.document().is_some());
        copy.open_copy_from(&source, None);

        let source_ref = source.downgrade();
        contents.remove(&source);
        drop(source);
        wait_until("source document disposal", || {
            source_ref.upgrade().is_none()
        });
        assert!(downloaded.query_exists(gio::Cancellable::NONE));

        let copy_ref = copy.downgrade();
        contents.remove(&copy);
        drop(copy);
        wait_until("document copy disposal", || copy_ref.upgrade().is_none());
        assert!(!downloaded.query_exists(gio::Cancellable::NONE));
        window.destroy();
    });
}

#[test]
fn closing_or_canceling_a_load_releases_its_pending_work() {
    crate::tests::run(|| {
        let loading = PpsDocumentTab::new();
        let window = adw::Window::builder().content(&loading).build();
        window.present();
        loading.open(&fixture("PasswordEncrypted.pdf"), None, None);
        assert!(loading.loading());
        loading.request_close();
        assert!(!loading.loading());
        let loading_ref = loading.downgrade();
        window.set_content(gtk::Widget::NONE);
        drop(loading);
        wait_until("closed loader disposal", || loading_ref.upgrade().is_none());

        let canceled = PpsDocumentTab::new();
        window.set_content(Some(&canceled));
        canceled.open(&fixture("PasswordEncrypted.pdf"), None, None);
        wait_until("password prompt", || {
            canceled.imp().mode.get() == WindowRunMode::PasswordView
                && window.visible_dialog().is_some()
        });
        window.visible_dialog().unwrap().force_close();
        wait_until("password cancellation", || !canceled.loading());
        assert!(canceled.is_empty());
        assert!(canceled.uri().is_none());

        canceled.open(&fixture("utf16le-annot.pdf"), None, None);
        wait_until("document after cancellation", || {
            canceled.document().is_some()
        });
        window.destroy();
    });
}
