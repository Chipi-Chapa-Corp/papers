use super::*;

#[test]
fn closing_reports_the_result_without_destroying_the_container() {
    gtk::test_synced(|| {
        let app = PpsApplication::new();
        app.register(gio::Cancellable::NONE).unwrap();

        let view: PpsDocumentView = glib::Object::new();
        let window = adw::Window::builder().content(&view).build();
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

        let file = gio::File::for_path(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../test-data/utf16le-annot.pdf"
        ));
        let job = papers_view::JobLoad::new();
        job.set_uri(&file.uri());
        job.run();
        job.is_succeeded().unwrap();
        let document = job.loaded_document().unwrap();
        view.open_document(&document, None, None, WindowRunMode::Normal);

        let annotation = AnnotationText::new(&document.page(0).unwrap());
        document
            .dynamic_cast_ref::<DocumentAnnotations>()
            .unwrap()
            .add_annotation(&annotation);

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
