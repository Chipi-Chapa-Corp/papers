use crate::deps::*;
use std::path::PathBuf;

use glib::UserDirectory;

use papers_document::LinkDest;

use crate::application::spawn;
use crate::config::PROFILE;

#[cfg(test)]
mod tests;

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub enum WindowRunMode {
    #[default]
    StartView,
    Normal,
    Fullscreen,
    Presentation,
    LoaderView,
    ErrorView,
    PasswordView,
}

mod imp {
    use super::*;

    #[derive(Default, Debug, CompositeTemplate)]
    #[template(resource = "/org/gnome/papers/ui/window.ui")]
    pub struct PpsWindow {
        #[template_child]
        pub(super) document_tab: TemplateChild<PpsDocumentTab>,
        #[template_child]
        pub(super) settings: TemplateChild<gio::Settings>,
        #[template_child]
        pub(super) default_settings: TemplateChild<gio::Settings>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for PpsWindow {
        const NAME: &'static str = "PpsWindow";
        type Type = super::PpsWindow;
        type ParentType = adw::ApplicationWindow;

        fn class_init(klass: &mut Self::Class) {
            gdk::FileList::ensure_type();
            PpsDocumentTab::ensure_type();
            klass.bind_template();
            klass.bind_template_callbacks();
        }
        fn instance_init(obj: &InitializingObject<Self>) {
            obj.init_template();
        }
    }

    impl ObjectImpl for PpsWindow {
        fn constructed(&self) {
            self.parent_constructed();
            #[allow(clippy::const_is_empty)]
            if !PROFILE.is_empty() {
                self.obj().add_css_class("devel");
            }
            self.setup_actions();
            self.document_tab
                .bind_property("title", self.obj().as_ref(), "title")
                .sync_create()
                .build();
            self.obj()
                .change_action_state("night-mode", &self.settings.boolean("night-mode").into());
        }
        fn dispose(&self) {
            self.default_settings.apply();
        }
    }

    impl WidgetImpl for PpsWindow {}
    impl WindowImpl for PpsWindow {
        fn close_request(&self) -> glib::Propagation {
            self.document_tab.request_close();
            glib::Propagation::Stop
        }
    }
    impl ApplicationWindowImpl for PpsWindow {}
    impl AdwApplicationWindowImpl for PpsWindow {}

    impl PpsWindow {
        fn setup_actions(&self) {
            let actions = [
                gio::ActionEntryBuilder::new("open")
                    .activate(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, _, _| {
                            obj.cmd_file_open();
                        }
                    ))
                    .build(),
                gio::ActionEntryBuilder::new("close")
                    .activate(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, _, _| {
                            obj.obj().close();
                        }
                    ))
                    .build(),
                gio::ActionEntryBuilder::new("fullscreen")
                    .state(false.into())
                    .change_state(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, action, state| {
                            let enabled = state.and_then(|v| v.get::<bool>()).unwrap();

                            obj.document_tab.set_fullscreen_mode(enabled);

                            if enabled {
                                obj.obj().fullscreen();
                            } else {
                                obj.obj().unfullscreen();
                            }

                            action.set_state(state.unwrap());
                        }
                    ))
                    .build(),
                gio::ActionEntryBuilder::new("escape")
                    .activate(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, _, _| {
                            obj.document_tab.escape();
                        }
                    ))
                    .build(),
                gio::ActionEntryBuilder::new("night-mode")
                    .state(false.into())
                    .change_state(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, action, state| {
                            let state = state.unwrap();
                            let night_mode = state.get::<bool>().unwrap();

                            action.set_state(state);

                            obj.set_night_mode(night_mode);
                        }
                    ))
                    .build(),
                gio::ActionEntryBuilder::new("presentation")
                    .activate(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, _, _| {
                            obj.document_tab.present_document();
                        }
                    ))
                    .build(),
            ];

            self.obj().add_action_entries(actions);
        }

        fn setup_window_size(&self) {
            // HACK: Mutte seems send a configure event of the original size
            // if we set the window size immediately after the dialog is closed
            let window = self.obj().clone();

            glib::timeout_add_local_once(
                std::time::Duration::from_millis(100),
                glib::clone!(
                    #[weak(rename_to = obj)]
                    self,
                    move || {
                        obj.default_settings.delay();
                        obj.default_settings
                            .bind("window-width", &window, "default-width")
                            .build();
                        obj.default_settings
                            .bind("window-height", &window, "default-height")
                            .build();
                        obj.default_settings
                            .bind("window-maximized", &window, "maximized")
                            .build();
                        // When the window is resized, the sidebar may incorrectly grab the focus
                        glib::timeout_add_local_once(
                            std::time::Duration::from_millis(100),
                            glib::clone!(
                                #[weak(rename_to = obj)]
                                obj,
                                move || {
                                    if !obj.document_tab.is_presenting() {
                                        obj.document_tab.focus_view()
                                    }
                                }
                            ),
                        );
                    }
                ),
            );
        }

        fn set_night_mode(&self, night_mode: bool) {
            self.document_tab.set_inverted_colors(night_mode);

            let manager = adw::StyleManager::for_display(&WidgetExt::display(&self.obj().clone()));

            let color_scheme = if night_mode {
                adw::ColorScheme::ForceDark
            } else {
                adw::ColorScheme::Default
            };

            manager.set_color_scheme(color_scheme);

            self.settings
                .set_boolean("night-mode", night_mode)
                .expect("failed to save night-mode");
        }

        fn settings_key_for_directory(dir: UserDirectory) -> String {
            match dir {
                UserDirectory::Pictures => "pictures-directory",
                _ => "document-directory",
            }
            .into()
        }

        fn file_dialog_restore_folder(&self, dialog: &gtk::FileDialog, dir: UserDirectory) {
            let settings = self.settings.get();
            let key = Self::settings_key_for_directory(dir);

            let folder = settings.get::<Option<String>>(&key).map(PathBuf::from);
            let folder = folder
                .or_else(|| glib::user_special_dir(dir))
                .unwrap_or_else(glib::home_dir);

            dialog.set_initial_folder(Some(&gio::File::for_path(folder)));
        }

        fn file_dialog_save_folder(&self, file: Option<&gio::File>, dir: UserDirectory) {
            let folder = file.and_then(|f| f.parent());

            // store 'nothing' if the folder is the default one
            let path = folder
                .filter(|f| f.path() != glib::user_special_dir(dir))
                .and_then(|f| f.path())
                .and_then(|path| path.into_os_string().into_string().ok());

            let settings = self.settings.get();
            let key = Self::settings_key_for_directory(dir);

            settings
                .set(&key, path)
                .expect("Failed to save folder path");
        }

        fn cmd_file_open(&self) {
            let dialog = gtk::FileDialog::builder().modal(true).build();

            papers_document::Document::factory_add_filters(&dialog, Document::NONE);

            self.file_dialog_restore_folder(&dialog, UserDirectory::Documents);

            glib::spawn_future_local(glib::clone!(
                #[weak(rename_to = obj)]
                self,
                async move {
                    let Ok(files) = dialog.open_multiple_future(Some(obj.obj().as_ref())).await
                    else {
                        return;
                    };

                    for f in files.iter::<gio::File>() {
                        let f = f.unwrap();
                        let uri = f.uri();

                        if obj.obj().uri().is_some_and(|u| u != uri) {
                            spawn(Some(&f), None, None);
                        } else {
                            obj.open(&f, None, None);
                        }
                    }

                    if files.n_items() > 0 {
                        let file = files.item(0).and_downcast::<gio::File>();

                        obj.file_dialog_save_folder(file.as_ref(), UserDirectory::Documents);
                    }
                }
            ));
        }

        pub(super) fn open(
            &self,
            file: &gio::File,
            dest: Option<&LinkDest>,
            mode: Option<WindowRunMode>,
        ) {
            if self.document_tab.is_empty() {
                self.setup_window_size();
            }
            self.document_tab.open(file, dest, mode);
        }
        fn open_copy(&self, source: &PpsDocumentTab, dest: Option<&LinkDest>) {
            let win = super::PpsWindow::new();
            win.imp().document_tab.open_copy_from(source, dest);
            win.set_default_size(self.obj().width(), self.obj().height());
            win.present();
        }
    }

    #[gtk::template_callbacks]
    impl PpsWindow {
        #[template_callback]
        fn document_tab_open_copy(&self, dest: Option<LinkDest>, source: &PpsDocumentTab) {
            self.open_copy(source, dest.as_ref());
        }
        #[template_callback]
        fn document_tab_close_finished(&self, allowed: bool) {
            if allowed {
                self.obj().destroy();
            }
        }
        #[template_callback]
        fn window_fullscreened(&self) {
            let obj = self.obj();

            if !obj.is_fullscreen() {
                obj.change_action_state("fullscreen", &false.into());
            }
        }

        #[template_callback]
        fn night_mode_changed(&self) {
            let night_mode = self.settings.boolean("night-mode");

            let current = self
                .obj()
                .action_state("night-mode")
                .unwrap()
                .get::<bool>()
                .unwrap();

            if night_mode != current {
                self.obj()
                    .change_action_state("night-mode", &night_mode.into());
            }
        }

        #[template_callback]
        fn drag_data_received(&self, value: glib::BoxedValue) -> bool {
            let Ok(file_list) = value.get_owned::<gdk::FileList>() else {
                return false;
            };

            for file in file_list.files() {
                let uri = file.uri();
                let current_uri = self.document_tab.uri();

                // Only open the file if we don't have an uri, or if it's
                // different to our current one. Don't reload the current open
                // document!
                if let Some(current_uri) = current_uri {
                    if current_uri != uri {
                        crate::application::spawn(Some(&file), None, None);
                    }
                } else {
                    self.open(&file, None, None);
                }
            }

            true
        }
    }
}

glib::wrapper! {
    pub struct PpsWindow(ObjectSubclass<imp::PpsWindow>)
        @extends gtk::Widget, gtk::Window, gtk::ApplicationWindow, adw::ApplicationWindow,
        @implements gtk::Native, gio::ActionGroup, gio::ActionMap, gtk::Accessible, gtk::Buildable, gtk::ConstraintTarget, gtk::Root, gtk::ShortcutManager;
}

impl PpsWindow {
    pub fn new() -> Self {
        glib::Object::builder()
            .property("application", gio::Application::default())
            .property("show-menubar", false)
            .build()
    }

    pub fn uri(&self) -> Option<String> {
        self.imp().document_tab.uri()
    }

    pub fn open(
        &self,
        file: &gio::File,
        dest: Option<&papers_document::LinkDest>,
        mode: Option<WindowRunMode>,
    ) {
        self.imp().open(file, dest, mode)
    }

    pub fn document(&self) -> Option<papers_document::Document> {
        self.imp().document_tab.document()
    }

    pub fn is_empty(&self) -> bool {
        self.imp().document_tab.is_empty()
    }

    pub fn metadata(&self) -> Option<papers_view::Metadata> {
        self.imp().document_tab.metadata()
    }

    pub fn print_range(&self, first_page: i32, last_page: i32) {
        self.imp().document_tab.print_range(first_page, last_page);
    }
}

impl Default for PpsWindow {
    fn default() -> Self {
        Self::new()
    }
}
