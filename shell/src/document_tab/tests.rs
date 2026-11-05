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
        });
        assert!(!document.loading());
        assert!(encrypted.loading());
        assert!(encrypted.document().is_none());
        assert_eq!(document.display_name(), "utf16le-annot.pdf");

        encrypted
            .imp()
            .password_view
            .emit_by_name::<()>("unlock", &[&"password", &gio::PasswordSave::Never]);
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
