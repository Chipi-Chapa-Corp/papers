use crate::deps::*;
use glib::closure_local;
use std::cell::Cell;
use std::path::PathBuf;

use glib::UserDirectory;

use papers_document::LinkDest;

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
        pub(super) tabs: TemplateChild<adw::TabView>,
        #[template_child]
        pub(super) tab_bar: TemplateChild<adw::TabBar>,
        #[template_child]
        pub(super) tab_sidebar: TemplateChild<gtk::Box>,
        #[template_child]
        pub(super) tab_list: TemplateChild<gtk::ListView>,
        pub(super) narrow: Cell<bool>,
        pub(super) selected_tab: RefCell<Option<PpsDocumentTab>>,
        pub(super) selected_bindings: RefCell<Vec<glib::Binding>>,
        pub(super) closing: Cell<bool>,
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
            self.setup_tabs();
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
            self.closing.set(true);
            self.close_next_tab();
            glib::Propagation::Stop
        }
    }
    impl ApplicationWindowImpl for PpsWindow {}
    impl AdwApplicationWindowImpl for PpsWindow {}

    impl PpsWindow {
        fn setup_actions(&self) {
            let actions = [
                gio::ActionEntryBuilder::new("new-tab")
                    .activate(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, _, _| {
                            obj.new_tab();
                        }
                    ))
                    .build(),
                gio::ActionEntryBuilder::new("close-window")
                    .activate(glib::clone!(
                        #[weak(rename_to = obj)]
                        self,
                        move |_, _, _| {
                            obj.obj().close();
                        }
                    ))
                    .build(),
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
                            if let Some(page) = obj.tabs.selected_page() {
                                obj.tabs.close_page(&page);
                            }
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

                            if let Some(tab) = obj.current_tab() {
                                tab.set_fullscreen_mode(enabled);
                            }

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
                            if let Some(tab) = obj.current_tab() {
                                tab.escape();
                            }
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
                            if let Some(tab) = obj.current_tab() {
                                tab.present_document();
                            }
                        }
                    ))
                    .build(),
            ];

            self.obj().add_action_entries(actions);
            self.obj()
                .add_action(&self.settings.create_action("vertical-tabs"));
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
                                    if let Some(tab) = obj.current_tab() {
                                        tab.focus_view();
                                    }
                                }
                            ),
                        );
                    }
                ),
            );
        }

        fn set_night_mode(&self, night_mode: bool) {
            for page in self.tabs.pages().iter::<adw::TabPage>().flatten() {
                page.child()
                    .downcast::<PpsDocumentTab>()
                    .unwrap()
                    .set_inverted_colors(night_mode);
            }

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
                        obj.open(&f, None, None);
                    }

                    if files.n_items() > 0 {
                        let file = files.item(0).and_downcast::<gio::File>();

                        obj.file_dialog_save_folder(file.as_ref(), UserDirectory::Documents);
                    }
                }
            ));
        }

        pub(super) fn current_tab(&self) -> Option<PpsDocumentTab> {
            self.tabs
                .selected_page()
                .map(|page| page.child().downcast().unwrap())
        }

        fn setup_tabs(&self) {
            self.tab_list.set_model(Some(&self.tabs.pages()));
            self.tab_list
                .set_factory(Some(&crate::tab_list::factory(&self.tabs)));
            self.update_tab_layout();
            // These shortcuts already navigate within the document.
            self.tabs.remove_shortcuts(
                adw::TabViewShortcuts::CONTROL_HOME
                    | adw::TabViewShortcuts::CONTROL_END
                    | adw::TabViewShortcuts::CONTROL_PAGE_UP
                    | adw::TabViewShortcuts::CONTROL_PAGE_DOWN,
            );
            self.tabs.connect_close_page(|_, page| {
                page.child()
                    .downcast::<PpsDocumentTab>()
                    .unwrap()
                    .request_close();
                glib::Propagation::Stop
            });
            self.tabs.connect_selected_page_notify(glib::clone!(
                #[weak(rename_to = obj)]
                self,
                move |_| obj.selected_page_changed()
            ));
            self.tabs.connect_page_detached(glib::clone!(
                #[weak(rename_to = obj)]
                self,
                move |view, _, _| {
                    if view.n_pages() == 0 {
                        obj.obj().destroy();
                    }
                }
            ));
            self.tabs.connect_create_window(|_| {
                let window = super::PpsWindow::new();
                window.present();
                Some(window.imp().tabs.get())
            });
        }

        fn selected_page_changed(&self) {
            let tab = self.current_tab();
            if let Some(previous) = self.selected_tab.replace(tab.clone())
                && previous.is_presenting()
            {
                previous.escape();
                self.obj().unfullscreen();
                self.obj()
                    .lookup_action("fullscreen")
                    .and_downcast::<gio::SimpleAction>()
                    .unwrap()
                    .set_enabled(true);
            }
            for binding in self.selected_bindings.borrow_mut().drain(..) {
                binding.unbind();
            }
            if let Some(tab) = tab {
                self.selected_bindings.borrow_mut().push(
                    tab.bind_property("title", self.obj().as_ref(), "title")
                        .sync_create()
                        .build(),
                );
                tab.set_fullscreen_mode(self.obj().is_fullscreen());
                tab.focus_view();
            } else {
                self.obj().set_title(Some(&gettext("Document Viewer")));
            }
            self.update_tab_layout();
        }

        fn update_tab_layout(&self) {
            let presenting = self.current_tab().is_some_and(|tab| tab.presenting());
            let vertical = self.settings.boolean("vertical-tabs") && !self.narrow.get();
            self.tab_sidebar.set_visible(vertical && !presenting);
            self.tab_bar.set_visible(!vertical && !presenting);
        }

        fn append_tab(&self, tab: &PpsDocumentTab) -> adw::TabPage {
            tab.connect_closure(
                "close-finished",
                false,
                closure_local!(move |tab: PpsDocumentTab, allowed: bool| {
                    if let Some(window) = tab.root().and_downcast::<super::PpsWindow>() {
                        window.imp().finish_close(&tab, allowed);
                    }
                }),
            );
            tab.connect_closure(
                "open-copy",
                false,
                closure_local!(move |tab: PpsDocumentTab, dest: Option<LinkDest>| {
                    if let Some(window) = tab.root().and_downcast::<super::PpsWindow>() {
                        window.imp().open_copy(&tab, dest.as_ref());
                    }
                }),
            );
            tab.connect_presenting_notify(|tab| {
                if let Some(window) = tab.root().and_downcast::<super::PpsWindow>() {
                    window.imp().update_tab_layout();
                }
            });
            let page = self.tabs.append(tab);
            tab.bind_property("display-name", &page, "title")
                .transform_to(|_, name: String| {
                    Some(if name.is_empty() {
                        gettext("New Tab")
                    } else {
                        name
                    })
                })
                .sync_create()
                .build();
            tab.bind_property("loading", &page, "loading")
                .sync_create()
                .build();
            tab.bind_property("modified", &page, "indicator-icon")
                .transform_to(|_, modified: bool| {
                    Some(modified.then(|| {
                        gio::ThemedIcon::new("document-edit-symbolic").upcast::<gio::Icon>()
                    }))
                })
                .sync_create()
                .build();
            page.set_indicator_tooltip(&gettext("Unsaved Changes"));
            self.tabs.set_selected_page(&page);
            page
        }

        fn finish_close(&self, tab: &PpsDocumentTab, allowed: bool) {
            let page = self.tabs.page(tab);
            if !allowed {
                self.closing.set(false);
            }
            self.tabs.close_page_finish(&page, allowed);
            if self.closing.get() {
                glib::idle_add_local_once(glib::clone!(
                    #[weak(rename_to = obj)]
                    self,
                    move || obj.close_next_tab()
                ));
            }
        }

        fn close_next_tab(&self) {
            if self.tabs.n_pages() == 0 {
                self.obj().destroy();
            } else {
                let page = self.tabs.nth_page(0);
                self.tabs.set_selected_page(&page);
                self.tabs.close_page(&page);
            }
        }

        pub(super) fn new_tab(&self) -> PpsDocumentTab {
            if self.tabs.n_pages() == 0 {
                self.setup_window_size();
            }
            let tab = PpsDocumentTab::new();
            self.append_tab(&tab);
            tab
        }

        pub(super) fn open(
            &self,
            file: &gio::File,
            dest: Option<&LinkDest>,
            mode: Option<WindowRunMode>,
        ) {
            for page in self.tabs.pages().iter::<adw::TabPage>().flatten() {
                let tab = page.child().downcast::<PpsDocumentTab>().unwrap();
                if tab.uri().as_deref() == Some(file.uri().as_str()) {
                    self.tabs.set_selected_page(&page);
                    tab.open(file, dest, mode);
                    return;
                }
            }
            let tab = self
                .current_tab()
                .filter(|tab| tab.is_empty())
                .unwrap_or_else(|| self.new_tab());
            tab.open(file, dest, mode);
        }

        fn open_copy(&self, source: &PpsDocumentTab, dest: Option<&LinkDest>) {
            let win = super::PpsWindow::new();
            win.new_tab().open_copy_from(source, dest);
            win.set_default_size(self.obj().width(), self.obj().height());
            win.present();
        }
    }

    #[gtk::template_callbacks]
    impl PpsWindow {
        #[template_callback]
        fn vertical_tabs_changed(&self) {
            self.update_tab_layout();
        }

        #[template_callback]
        fn narrow_layout(&self) {
            self.narrow.set(true);
            self.update_tab_layout();
        }

        #[template_callback]
        fn wide_layout(&self) {
            self.narrow.set(false);
            self.update_tab_layout();
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
                self.open(&file, None, None);
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

    pub fn new_tab(&self) -> PpsDocumentTab {
        self.imp().new_tab()
    }

    pub fn uri(&self) -> Option<String> {
        self.imp().current_tab().and_then(|tab| tab.uri())
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
        self.imp().current_tab().and_then(|tab| tab.document())
    }

    pub fn is_empty(&self) -> bool {
        self.imp().current_tab().is_none_or(|tab| tab.is_empty())
    }

    pub fn metadata(&self) -> Option<papers_view::Metadata> {
        self.imp().current_tab().and_then(|tab| tab.metadata())
    }

    pub fn print_range(&self, first_page: i32, last_page: i32) {
        if let Some(tab) = self.imp().current_tab() {
            tab.print_range(first_page, last_page);
        }
    }
}

impl Default for PpsWindow {
    fn default() -> Self {
        Self::new()
    }
}
