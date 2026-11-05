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
        assert!(window.imp().document_tab.is_presenting());
        WidgetExt::activate_action(&window, "win.escape", None).unwrap();
        assert!(!window.imp().document_tab.is_presenting());
        assert!(window.lookup_action("fullscreen").unwrap().is_enabled());

        window.close();
        assert!(!window.is_visible());
    });
}
