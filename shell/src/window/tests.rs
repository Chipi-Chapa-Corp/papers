use super::*;
use crate::tests::{fixture, wait_until};

#[test]
fn window_actions_follow_the_document_lifecycle() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        window.present();
        wait_until("window document", || window.document().is_some());
        assert_eq!(window.title().as_deref(), Some("utf16le-annot.pdf"));

        WidgetExt::activate_action(&window, "win.presentation", None).unwrap();
        assert!(window.imp().current_tab().unwrap().is_presenting());
        WidgetExt::activate_action(&window, "win.escape", None).unwrap();
        assert!(!window.imp().current_tab().unwrap().is_presenting());
        assert!(window.lookup_action("fullscreen").unwrap().is_enabled());

        window.close();
        assert!(!window.is_visible());
    });
}

#[test]
fn repeated_open_requests_keep_the_document_and_apply_the_requested_mode() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        let file = fixture("utf16le-annot.pdf");
        window.open(&file, None, None);
        window.open(&file, None, Some(WindowRunMode::Fullscreen));
        window.present();
        wait_until("fullscreen document", || window.document().is_some());
        assert!(
            window
                .action_state("fullscreen")
                .unwrap()
                .get::<bool>()
                .unwrap()
        );

        let document = window.document();
        window.open(&file, None, Some(WindowRunMode::Fullscreen));
        assert_eq!(window.document(), document);
        assert_eq!(window.imp().tabs.n_pages(), 1);
        assert!(
            window
                .action_state("fullscreen")
                .unwrap()
                .get::<bool>()
                .unwrap()
        );
        window.close();
    });
}

#[test]
fn tabs_reorder_transfer_and_close_through_their_current_window() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        let first = window.imp().current_tab().unwrap();
        window.open(&fixture("ink_annotations.pdf"), None, None);
        let second = window.imp().current_tab().unwrap();
        window.present();
        wait_until("both documents", || {
            first.document().is_some() && second.document().is_some()
        });
        let tabs = window.imp().tabs.get();
        assert_eq!(tabs.n_pages(), 2);
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        assert_eq!(tabs.n_pages(), 2);
        assert_eq!(window.imp().current_tab(), Some(first.clone()));
        assert_eq!(window.title().as_deref(), Some("utf16le-annot.pdf"));

        let page = tabs.page(&first);
        tabs.reorder_page(&page, 1);
        assert_eq!(tabs.nth_page(1), page);
        WidgetExt::activate_action(&window, "win.presentation", None).unwrap();
        assert!(first.is_presenting());
        let destination = PpsWindow::new();
        destination.present();
        tabs.transfer_page(&page, &destination.imp().tabs, 0);
        assert_eq!(tabs.n_pages(), 1);
        assert_eq!(destination.imp().tabs.nth_page(0), page);
        assert_eq!(destination.imp().current_tab(), Some(first));
        assert_eq!(window.imp().current_tab(), Some(second.clone()));
        assert!(!destination.imp().current_tab().unwrap().is_presenting());
        assert!(window.lookup_action("fullscreen").unwrap().is_enabled());

        WidgetExt::activate_action(&destination, "win.close", None).unwrap();
        assert!(!destination.is_visible());
        assert!(window.is_visible());
        assert_eq!(tabs.n_pages(), 1);

        let document = second.document().unwrap();
        let annotation = papers_document::AnnotationText::new(&document.page(0).unwrap());
        document
            .dynamic_cast_ref::<papers_document::DocumentAnnotations>()
            .unwrap()
            .add_annotation(&annotation);
        assert!(tabs.page(&second).indicator_icon().is_some());
        window.new_tab();
        window.close();
        window.visible_dialog().unwrap().force_close();
        wait_until("canceled window close", || !window.imp().closing.get());
        assert!(window.is_visible());
        assert_eq!(tabs.n_pages(), 2);
        assert_eq!(window.imp().current_tab(), Some(second));

        window.close();
        let dialog = window
            .visible_dialog()
            .and_downcast::<adw::AlertDialog>()
            .unwrap();
        dialog.emit_by_name::<()>("response", &[&"no"]);
        wait_until("window close", || !window.is_visible());
    });
}
