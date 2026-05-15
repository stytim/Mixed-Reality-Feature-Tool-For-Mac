#include <wx/wxprec.h>
#ifndef WX_PRECOMP
    #include <wx/wx.h>
#endif
#include <wx/dirdlg.h>
#include <wx/simplebook.h>
#include <wx/scrolwin.h>
#include <wx/statline.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <curl/curl.h>

#include "core_logic.h"

// --- Custom Events for Thread Communication ---
wxDEFINE_EVENT(wxEVT_DISCOVERY_COMPLETE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_DISCOVERY_FAILED, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_RESOLVE_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_INSTALL_UPDATE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_INSTALL_COMPLETE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_WORKER_ERROR, wxThreadEvent);

// --- Forward Declarations ---
class MyFrame;
class ProgressPanel;
class ProjectSelectPanel;

// Shared shutdown flag — survives MyFrame so detached threads can check before posting events.
// All worker threads hold a copy of this shared_ptr; main thread sets *flag=true on close.
using ShutdownFlag = std::shared_ptr<std::atomic<bool>>;

// --- Main Application Class ---
class MyApp : public wxApp {
public:
    virtual bool OnInit();
    virtual int OnExit();
};

// --- Main Frame Class ---
class MyFrame : public wxFrame {
public:
    MyFrame();
    ~MyFrame() override;

    void StartInstallation();
    void OnStartOver(wxCommandEvent& event);

    fs::path projectPath;
    MRTKToolCore tool;
    wxSimplebook* book;
    ShutdownFlag shutdownFlag = std::make_shared<std::atomic<bool>>(false);

private:
    void OnDiscoveryComplete(wxThreadEvent& event);
    void OnDiscoveryFailed(wxThreadEvent& event);
    void OnResolveComplete(wxCommandEvent& event);
    void OnInstallUpdate(wxThreadEvent& event);
    void OnInstallComplete(wxThreadEvent& event);
    void OnWorkerError(wxThreadEvent& event);
    void OnClose(wxCloseEvent& event);

    ProgressPanel* progressPanel = nullptr;
};

// --- Panel Classes ---
wxSizer* CreatePackageListSizer(wxWindow* parent, const std::map<std::string, std::string>& packages) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    wxFont itemFont = parent->GetFont();
    itemFont.SetPointSize(itemFont.GetPointSize() + 1);

    for (const auto& [name, version] : packages) {
        auto* itemSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* cb = new wxCheckBox(parent, wxID_ANY, "");
        cb->SetValue(true);
        cb->Disable();
        auto* label = new wxStaticText(parent, wxID_ANY, name + " " + version);
        label->SetFont(itemFont);
        label->SetForegroundColour(*wxWHITE);
        itemSizer->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        itemSizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(itemSizer, 0, wxEXPAND | wxBOTTOM, 5);
    }
    return sizer;
}

class ProjectSelectPanel : public wxPanel {
public:
    ProjectSelectPanel(wxWindow* parent, MyFrame* mainFrame);
    void ResetDiscoverButton();
private:
    void OnBrowse(wxCommandEvent&);
    void OnPathChanged(wxCommandEvent&);
    void OnDiscover(wxCommandEvent&);
    void ValidateProjectPath();
    MyFrame* m_mainFrame;
    wxTextCtrl* pathTextCtrl;
    wxStaticText* versionText;
    wxButton* discoverButton;
};

class FeatureSelectPanel : public wxPanel {
public:
    FeatureSelectPanel(wxWindow* parent, MyFrame* mainFrame, const std::vector<SelectablePackage>& packages);
private:
    void OnGetFeatures(wxCommandEvent&);
    MyFrame* m_mainFrame;
    std::vector<wxCheckBox*> checkBoxes;
};

class ImportPanel : public wxPanel {
public:
    ImportPanel(wxWindow* parent, MyFrame* mainFrame);
private:
    void OnImport(wxCommandEvent& event) {
        m_mainFrame->book->SetSelection(3);
        m_mainFrame->StartInstallation();
    }
    void OnGoBack(wxCommandEvent& event) {
        m_mainFrame->book->SetSelection(1);
    }
    MyFrame* m_mainFrame;
};

class ProgressPanel : public wxPanel {
public:
    ProgressPanel(wxWindow* parent) : wxPanel(parent) {
        SetBackgroundColour(wxColour(40, 40, 40));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(this, wxID_ANY, "Importing Features");
        wxFont titleFont(24, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        title->SetFont(titleFont);
        title->SetForegroundColour(*wxWHITE);

        logOutput = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        logOutput->SetBackgroundColour(*wxBLACK);
        logOutput->SetForegroundColour(wxColour(200, 200, 200));

        sizer->Add(title, 0, wxALL, 20);
        sizer->Add(logOutput, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 20);
        SetSizer(sizer);
    }
    void AppendLog(const wxString& text) {
        logOutput->AppendText(text);
    }
private:
    wxTextCtrl* logOutput;
};

class CompletionPanel : public wxPanel {
public:
    CompletionPanel(wxWindow* parent, MyFrame* mainFrame) : wxPanel(parent) {
        SetBackgroundColour(wxColour(40, 40, 40));
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* title = new wxStaticText(this, wxID_ANY, "Unity Project Updated");
        wxFont titleFont(24, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        title->SetFont(titleFont);
        title->SetForegroundColour(*wxWHITE);

        std::string message = mainFrame->projectPath.filename().string() + " has been updated.\nPlease return to Unity to load the imported features.";
        auto* body = new wxStaticText(this, wxID_ANY, message);
        body->SetForegroundColour(wxColour(220, 220, 220));

        auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* startOverBtn = new wxButton(this, wxID_ANY, "Start Over");
        auto* exitBtn = new wxButton(this, wxID_EXIT, "Exit");

        btnSizer->Add(startOverBtn, 0, wxRIGHT, 10);
        btnSizer->Add(exitBtn);

        sizer->Add(title, 0, wxALL, 20);
        sizer->Add(body, 0, wxLEFT | wxRIGHT | wxBOTTOM, 20);
        sizer->AddStretchSpacer(1);
        sizer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, 20);
        SetSizer(sizer);

        startOverBtn->Bind(wxEVT_BUTTON, &MyFrame::OnStartOver, mainFrame);
        exitBtn->Bind(wxEVT_BUTTON, [mainFrame](wxCommandEvent&) {
            mainFrame->Close();
        });
    }
};

// --- Implementation ---
wxIMPLEMENT_APP(MyApp);

bool MyApp::OnInit() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    MyFrame *frame = new MyFrame();
    frame->Show(true);
    return true;
}

int MyApp::OnExit() {
    curl_global_cleanup();
    return 0;
}

MyFrame::MyFrame() : wxFrame(NULL, wxID_ANY, "MRTK Feature Tool", wxDefaultPosition, wxSize(700, 550)) {
    SetBackgroundColour(wxColour(40, 40, 40));
    book = new wxSimplebook(this, wxID_ANY);
    book->AddPage(new ProjectSelectPanel(book, this), "Select Project");

    // Wire progress events through a marshaller so background threads don't touch widgets directly.
    ShutdownFlag flagCopy = shutdownFlag;
    tool.setProgressCallback([this, flagCopy](const std::string& msg) {
        if (flagCopy->load(std::memory_order_acquire)) return;
        auto* ev = new wxThreadEvent(wxEVT_INSTALL_UPDATE);
        ev->SetString(msg + "\n");
        wxQueueEvent(this, ev);
    });

    Bind(wxEVT_DISCOVERY_COMPLETE, &MyFrame::OnDiscoveryComplete, this);
    Bind(wxEVT_DISCOVERY_FAILED,   &MyFrame::OnDiscoveryFailed,   this);
    Bind(wxEVT_RESOLVE_COMPLETE,   &MyFrame::OnResolveComplete,   this);
    Bind(wxEVT_INSTALL_UPDATE,     &MyFrame::OnInstallUpdate,     this);
    Bind(wxEVT_INSTALL_COMPLETE,   &MyFrame::OnInstallComplete,   this);
    Bind(wxEVT_WORKER_ERROR,       &MyFrame::OnWorkerError,       this);
    Bind(wxEVT_CLOSE_WINDOW,       &MyFrame::OnClose,             this);
}

MyFrame::~MyFrame() {
    shutdownFlag->store(true, std::memory_order_release);
}

void MyFrame::OnClose(wxCloseEvent& event) {
    shutdownFlag->store(true, std::memory_order_release);
    event.Skip();   // Allow normal close handling.
}

void MyFrame::OnDiscoveryComplete(wxThreadEvent& event) {
    auto discoveredPackages = tool.getAvailablePackages();
    book->AddPage(new FeatureSelectPanel(book, this, discoveredPackages), "Select Features");
    book->SetSelection(1);
}

void MyFrame::OnDiscoveryFailed(wxThreadEvent& event) {
    wxMessageBox(event.GetString().IsEmpty() ? "Failed to discover any components." : event.GetString(),
                 "Discovery Failed", wxICON_ERROR);
    auto* psp = static_cast<ProjectSelectPanel*>(book->GetPage(0));
    if (psp) psp->ResetDiscoverButton();
}

void MyFrame::OnResolveComplete(wxCommandEvent& event) {
    book->AddPage(new ImportPanel(book, this), "Import Features");
    progressPanel = new ProgressPanel(book);
    book->AddPage(progressPanel, "Progress");
    book->AddPage(new CompletionPanel(book, this), "Complete");
    book->SetSelection(2);
}

void MyFrame::OnWorkerError(wxThreadEvent& event) {
    wxMessageBox("A background task failed:\n" + event.GetString(), "Error", wxICON_ERROR);
    auto* psp = static_cast<ProjectSelectPanel*>(book->GetPage(0));
    if (psp) psp->ResetDiscoverButton();
}

void MyFrame::StartInstallation() {
    ShutdownFlag flagCopy = shutdownFlag;
    std::thread worker([this, flagCopy]() {
        try {
            tool.downloadAndRepackage();
            tool.installPackagesToProject(projectPath);
            if (!flagCopy->load(std::memory_order_acquire)) {
                wxQueueEvent(this, new wxThreadEvent(wxEVT_INSTALL_COMPLETE));
            }
        } catch (const std::exception& e) {
            if (flagCopy->load(std::memory_order_acquire)) return;
            auto* ev = new wxThreadEvent(wxEVT_WORKER_ERROR);
            ev->SetString(wxString::FromUTF8(e.what()));
            wxQueueEvent(this, ev);
        }
    });
    worker.detach();
}

void MyFrame::OnInstallUpdate(wxThreadEvent& event) {
    if (progressPanel) {
        progressPanel->AppendLog(event.GetString());
    }
}

void MyFrame::OnInstallComplete(wxThreadEvent& event) {
    book->SetSelection(4);
}

void MyFrame::OnStartOver(wxCommandEvent& event) {
    while(book->GetPageCount() > 1) {
        book->DeletePage(book->GetPageCount() - 1);
    }
    progressPanel = nullptr;
    book->ChangeSelection(0);
}

// --- ProjectSelectPanel Implementation ---
ProjectSelectPanel::ProjectSelectPanel(wxWindow* parent, MyFrame* mainFrame)
    : wxPanel(parent), m_mainFrame(mainFrame) {
    SetBackgroundColour(wxColour(40, 40, 40));
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);
    wxFont titleFont(24, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    auto* titleLabel = new wxStaticText(this, wxID_ANY, "Select Project");
    titleLabel->SetFont(titleFont);
    titleLabel->SetForegroundColour(*wxWHITE);
    wxBoxSizer* pathSizer = new wxBoxSizer(wxHORIZONTAL);
    pathSizer->Add(new wxStaticText(this, wxID_ANY, "Project Path:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    pathTextCtrl = new wxTextCtrl(this, wxID_ANY, "");
    pathSizer->Add(pathTextCtrl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    auto* browseButton = new wxButton(this, wxID_ANY, "...", wxDefaultPosition, wxSize(40, -1));
    pathSizer->Add(browseButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    wxBoxSizer* versionSizer = new wxBoxSizer(wxHORIZONTAL);
    versionSizer->Add(new wxStaticText(this, wxID_ANY, "Unity Version:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    versionText = new wxStaticText(this, wxID_ANY, "<None>");
    versionSizer->Add(versionText, 1, wxALIGN_CENTER_VERTICAL);
    discoverButton = new wxButton(this, wxID_ANY, "Discover Features");
    discoverButton->Disable();
    mainSizer->Add(titleLabel, 0, wxEXPAND | wxALL, 20);
    mainSizer->Add(pathSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 20);
    mainSizer->Add(versionSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 20);
    mainSizer->AddStretchSpacer(1);
    mainSizer->Add(discoverButton, 0, wxALIGN_RIGHT | wxALL, 20);
    SetSizer(mainSizer);
    for (auto child : this->GetChildren()) {
        if (auto* staticChild = dynamic_cast<wxStaticText*>(child)) {
            staticChild->SetForegroundColour(wxColour(220, 220, 220));
        }
    }
    titleLabel->SetForegroundColour(*wxWHITE);
    browseButton->Bind(wxEVT_BUTTON, &ProjectSelectPanel::OnBrowse, this);
    pathTextCtrl->Bind(wxEVT_TEXT, &ProjectSelectPanel::OnPathChanged, this);
    discoverButton->Bind(wxEVT_BUTTON, &ProjectSelectPanel::OnDiscover, this);
}

void ProjectSelectPanel::ResetDiscoverButton() {
    discoverButton->SetLabel("Discover Features");
    discoverButton->Enable();
}

void ProjectSelectPanel::OnBrowse(wxCommandEvent& event) {
    wxDirDialog d(this, "Select a Unity Project Folder", "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (d.ShowModal() == wxID_OK) { pathTextCtrl->SetValue(d.GetPath()); }
}

void ProjectSelectPanel::OnPathChanged(wxCommandEvent& event) {
    ValidateProjectPath();
}

void ProjectSelectPanel::ValidateProjectPath() {
    fs::path p(pathTextCtrl->GetValue().ToStdString());
    if (MRTKToolCore::isValidUnityProject(p)) {
        versionText->SetLabel(MRTKToolCore::getUnityVersion(p));
        versionText->SetForegroundColour(wxColour(150, 255, 150));
        discoverButton->Enable();
        m_mainFrame->projectPath = p;
    } else {
        versionText->SetLabel("<Invalid Project Path>");
        versionText->SetForegroundColour(wxColour(255, 150, 150));
        discoverButton->Disable();
    }
    Layout();
}

void ProjectSelectPanel::OnDiscover(wxCommandEvent& event) {
    discoverButton->SetLabel("Discovering...");
    discoverButton->Disable();
    MyFrame* mf = m_mainFrame;
    ShutdownFlag flagCopy = mf->shutdownFlag;
    std::thread t([mf, flagCopy]() {
        try {
            bool ok = mf->tool.fetchAvailablePackages();
            if (flagCopy->load(std::memory_order_acquire)) return;
            if (ok && !mf->tool.getAvailablePackages().empty()) {
                wxQueueEvent(mf, new wxThreadEvent(wxEVT_DISCOVERY_COMPLETE));
            } else {
                auto* ev = new wxThreadEvent(wxEVT_DISCOVERY_FAILED);
                ev->SetString("No components were returned by the GitHub API.");
                wxQueueEvent(mf, ev);
            }
        } catch (const std::exception& e) {
            if (flagCopy->load(std::memory_order_acquire)) return;
            auto* ev = new wxThreadEvent(wxEVT_DISCOVERY_FAILED);
            ev->SetString(wxString::FromUTF8(e.what()));
            wxQueueEvent(mf, ev);
        }
    });
    t.detach();
}


// --- FeatureSelectPanel Implementation ---
FeatureSelectPanel::FeatureSelectPanel(wxWindow* parent, MyFrame* mainFrame, const std::vector<SelectablePackage>& packages)
    : wxPanel(parent), m_mainFrame(mainFrame) {
    SetBackgroundColour(wxColour(40, 40, 40));
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    auto* titleLabel = new wxStaticText(this, wxID_ANY, "Discover Features");
    wxFont titleFont(24, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    titleLabel->SetFont(titleFont);
    titleLabel->SetForegroundColour(*wxWHITE);
    mainSizer->Add(titleLabel, 0, wxEXPAND | wxALL, 20);

    auto* scrollWindow = new wxScrolled<wxPanel>(this, wxID_ANY);
    scrollWindow->SetScrollRate(0, 10);
    scrollWindow->SetBackgroundColour(wxColour(50, 50, 50));

    auto* contentSizer = new wxBoxSizer(wxVERTICAL);

    wxFont sectionFont = GetFont();
    sectionFont.MakeBold();

    auto* mrtkLabel = new wxStaticText(scrollWindow, wxID_ANY, "Mixed Reality Toolkit");
    mrtkLabel->SetFont(sectionFont);
    mrtkLabel->SetForegroundColour(*wxWHITE);
    contentSizer->Add(mrtkLabel, 0, wxLEFT | wxTOP, 10);
    contentSizer->Add(new wxStaticLine(scrollWindow), 0, wxEXPAND | wxALL, 5);
    auto* mrtkSizer = new wxBoxSizer(wxVERTICAL);
    contentSizer->Add(mrtkSizer, 0, wxEXPAND | wxALL, 5);

    auto* openxrLabel = new wxStaticText(scrollWindow, wxID_ANY, "OpenXR Runtimes");
    openxrLabel->SetFont(sectionFont);
    openxrLabel->SetForegroundColour(*wxWHITE);
    contentSizer->Add(openxrLabel, 0, wxLEFT | wxTOP, 20);
    contentSizer->Add(new wxStaticLine(scrollWindow), 0, wxEXPAND | wxALL, 5);
    auto* openxrSizer = new wxBoxSizer(wxVERTICAL);
    contentSizer->Add(openxrSizer, 0, wxEXPAND | wxALL, 5);

    wxFont itemFont = GetFont();
    itemFont.SetPointSize(itemFont.GetPointSize() + 2);

    for (const auto& pkg : packages) {
        auto* itemSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* cb = new wxCheckBox(scrollWindow, wxID_ANY, "");
        checkBoxes.push_back(cb);
        auto* label = new wxStaticText(scrollWindow, wxID_ANY, pkg.displayName);
        label->SetFont(itemFont);
        label->SetForegroundColour(*wxWHITE);
        itemSizer->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        itemSizer->Add(label, 1, wxALIGN_CENTER_VERTICAL);
        label->Bind(wxEVT_LEFT_DOWN, [cb](wxMouseEvent&) {
            cb->SetValue(!cb->GetValue());
        });
        if (pkg.type == PackageType::MRTK) mrtkSizer->Add(itemSizer, 0, wxEXPAND | wxALL, 5);
        else openxrSizer->Add(itemSizer, 0, wxEXPAND | wxALL, 5);
    }

    scrollWindow->SetSizer(contentSizer);
    mainSizer->Add(scrollWindow, 1, wxEXPAND | wxLEFT | wxRIGHT, 20);
    auto* getFeaturesButton = new wxButton(this, wxID_ANY, "Get Features");
    mainSizer->Add(getFeaturesButton, 0, wxALIGN_RIGHT | wxALL, 20);
    SetSizer(mainSizer);

    getFeaturesButton->Bind(wxEVT_BUTTON, &FeatureSelectPanel::OnGetFeatures, this);
}

void FeatureSelectPanel::OnGetFeatures(wxCommandEvent& event) {
    std::vector<int> selectedIndices;
    for (size_t i = 0; i < checkBoxes.size(); ++i) {
        if (checkBoxes[i]->IsChecked()) selectedIndices.push_back(static_cast<int>(i));
    }
    if (selectedIndices.empty()) {
        wxMessageBox("No features were selected.", "Warning", wxOK | wxICON_WARNING);
        return;
    }

    MyFrame* mainFrame = m_mainFrame;
    ShutdownFlag flagCopy = mainFrame->shutdownFlag;
    std::thread worker([mainFrame, selectedIndices, flagCopy]() {
        try {
            mainFrame->tool.resolveDependencies(selectedIndices);
            if (!flagCopy->load(std::memory_order_acquire)) {
                wxQueueEvent(mainFrame, new wxCommandEvent(wxEVT_RESOLVE_COMPLETE));
            }
        } catch (const std::exception& e) {
            if (flagCopy->load(std::memory_order_acquire)) return;
            auto* ev = new wxThreadEvent(wxEVT_WORKER_ERROR);
            ev->SetString(wxString::FromUTF8(e.what()));
            wxQueueEvent(mainFrame, ev);
        }
    });
    worker.detach();
}


// --- ImportPanel Implementation ---
ImportPanel::ImportPanel(wxWindow* parent, MyFrame* mainFrame) : wxPanel(parent), m_mainFrame(mainFrame) {
    SetBackgroundColour(wxColour(40, 40, 40));
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(this, wxID_ANY, "Import Features");
    wxFont titleFont(24, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    title->SetFont(titleFont);
    title->SetForegroundColour(*wxWHITE);

    auto* body = new wxStaticText(this, wxID_ANY, "The Mixed Reality Feature Tool has identified the packages that are required to import your chosen features.");
    body->SetForegroundColour(wxColour(220, 220, 220));
    body->Wrap(600);

    auto* gridSizer = new wxFlexGridSizer(2, 20, 20);
    gridSizer->AddGrowableCol(0, 1);
    gridSizer->AddGrowableCol(1, 1);

    auto* featuresLabel = new wxStaticText(this, wxID_ANY, "Features");
    featuresLabel->SetForegroundColour(wxColour(200, 200, 200));
    gridSizer->Add(featuresLabel, 0, wxBOTTOM, 5);

    auto* depsLabel = new wxStaticText(this, wxID_ANY, "Required dependencies");
    depsLabel->SetForegroundColour(wxColour(200, 200, 200));
    gridSizer->Add(depsLabel, 0, wxBOTTOM, 5);

    gridSizer->Add(CreatePackageListSizer(this, mainFrame->tool.resolvedUserSelections), 1, wxEXPAND);
    gridSizer->Add(CreatePackageListSizer(this, mainFrame->tool.resolvedDependencies), 1, wxEXPAND);

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* goBackBtn = new wxButton(this, wxID_ANY, "Go back");
    auto* importBtn = new wxButton(this, wxID_ANY, "Import");

    btnSizer->Add(goBackBtn);
    btnSizer->AddStretchSpacer(1);
    btnSizer->Add(importBtn);

    sizer->Add(title, 0, wxALL, 20);
    sizer->Add(body, 0, wxLEFT | wxRIGHT | wxBOTTOM, 20);
    sizer->Add(gridSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 20);
    sizer->Add(btnSizer, 0, wxEXPAND | wxALL, 20);
    SetSizer(sizer);

    goBackBtn->Bind(wxEVT_BUTTON, &ImportPanel::OnGoBack, this);
    importBtn->Bind(wxEVT_BUTTON, &ImportPanel::OnImport, this);
}
