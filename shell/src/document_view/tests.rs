use super::*;

#[test]
fn closing_reports_the_result_without_destroying_the_container() {
    crate::tests::run(|| {
        let view = PpsDocumentView::new();
        let window = adw::Window::builder()
            .title("Container")
            .content(&view)
            .build();
        window.present();

        let results = std::rc::Rc::new(RefCell::new(Vec::<bool>::new()));
        view.connect_local(
            "close-finished",
            false,
            glib::clone!(
                #[strong]
                results,
                move |args| {
                    results.borrow_mut().push(args[1].get().unwrap());
                    None
                }
            ),
        );

        view.request_close();
        assert_eq!(*results.borrow(), [true]);
        assert!(window.is_visible());

        let file = crate::tests::fixture("utf16le-annot.pdf");
        let job = papers_view::JobLoad::new();
        job.set_uri(&file.uri());
        job.run();
        job.is_succeeded().unwrap();
        let document = job.loaded_document().unwrap();
        view.set_filenames("utf16le-annot.pdf", "utf16le-annot.pdf");
        view.open_document(&document, None, None, WindowRunMode::Normal);
        assert_eq!(view.title(), "utf16le-annot.pdf");
        assert_eq!(window.title().as_deref(), Some("Container"));
        assert!(!view.modified());

        let copy = PpsDocumentView::new();
        let copy_window = adw::Window::builder().content(&copy).build();
        copy.open_copy_from(&view, None);
        assert_eq!(copy.model().document(), Some(document.clone()));
        copy.model().set_scale(1.75);
        assert_ne!(copy.model().scale(), view.model().scale());
        copy_window.destroy();

        let annotation = AnnotationText::new(&document.page(0).unwrap());
        document
            .dynamic_cast_ref::<DocumentAnnotations>()
            .unwrap()
            .add_annotation(&annotation);
        assert!(view.modified());

        view.request_close();
        let dialog = window.visible_dialog().unwrap();
        view.request_close();
        assert_eq!(window.visible_dialog(), Some(dialog.clone()));
        dialog.force_close();
        assert_eq!(*results.borrow(), [true, false]);
        assert!(window.is_visible());

        view.request_close();
        let dialog = window
            .visible_dialog()
            .and_downcast::<adw::AlertDialog>()
            .unwrap();
        dialog.emit_by_name::<()>("response", &[&"no"]);
        dialog.force_close();
        assert_eq!(*results.borrow(), [true, false, true]);
        assert!(window.is_visible());
        window.destroy();
    });
}
