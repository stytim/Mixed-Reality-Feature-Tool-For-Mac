#include <wx/wxprec.h>
#ifndef WX_PRECOMP
    #include <wx/wx.h>
#endif
#include <wx/dirdlg.h>
#include <wx/simplebook.h>
#include <wx/scrolwin.h>
#include <wx/statline.h>
#include <thread>
#include <vector>

#include "core_logic.h"

// --- Custom Events for Thread Communication ---
wxDEFINE_EVENT(wxEVT_DISCOVERY_COMPLETE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_RESOLVE_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_INSTALL_UPDATE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_INSTALL_COMPLETE, wxThreadEvent);

// --- Forward Declarations ---
class MyFrame;
class ProgressPanel;

// --- Main Application Class ---
class MyApp : public wxApp {
public:
    virtual bool OnInit();
};

// --- Main Frame Class ---
class MyFrame : public wxFrame {
public:
    MyFrame();
    
    void StartInstallation();
    void OnStartOver(wxCommandEvent& event);
    
    fs::path projectPath;
    MRTKToolCore tool;
    wxSimplebook* book;

private:
    void OnDiscoveryComplete(wxThreadEvent& event);
    void OnResolveComplete(wxCommandEvent& event);
    void OnInstallUpdate(wxThreadEvent& event);
    void OnInstallComplete(wxThreadEvent& event);
    
    ProgressPanel* progressPanel;
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
    ProjectSelectPanel(wxWindow* parent);
private:
    void OnBrowse(wxCommandEvent&);
    void OnPathChanged(wxCommandEvent&);
    void OnDiscover(wxCommandEvent&);
    void ValidateProjectPath();
    wxTextCtrl* pathTextCtrl;
    wxStaticText* versionText;
    wxButton* discoverButton;
};

class FeatureSelectPanel : public wxPanel {
public:
    FeatureSelectPanel(wxWindow* parent, const std::vector<SelectablePackage>& packages);
private:
    void OnGetFeatures(wxCommandEvent&);
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
        
        // <<< FIX: Bind the Exit button's click event to close the main window.
        exitBtn->Bind(wxEVT_BUTTON, [mainFrame](wxCommandEvent& event){
            mainFrame->Close();
        });
    }
};

// --- Implementation ---
wxIMPLEMENT_APP(MyApp);

bool MyApp::OnInit() {
    MyFrame *frame = new MyFrame();
    frame->Show(true);
    return true;
}

MyFrame::MyFrame() : wxFrame(NULL, wxID_ANY, "MRTK Feature Tool", wxDefaultPosition, wxSize(700, 550)) {
    SetBackgroundColour(wxColour(40, 40, 40));
    book = new wxSimplebook(this, wxID_ANY);
    book->AddPage(new ProjectSelectPanel(book), "Select Project");
    Bind(wxEVT_DISCOVERY_COMPLETE, &MyFrame::OnDiscoveryComplete, this);
    Bind(wxEVT_RESOLVE_COMPLETE, &MyFrame::OnResolveComplete, this);
    Bind(wxEVT_INSTALL_UPDATE, &MyFrame::OnInstallUpdate, this);
    Bind(wxEVT_INSTALL_COMPLETE, &MyFrame::OnInstallComplete, this);
}

void MyFrame::OnDiscoveryComplete(wxThreadEvent& event) {
    auto discoveredPackages = tool.getAvailablePackages();
    if (discoveredPackages.empty()) {
        wxMessageBox("Failed to discover any components.", "Discovery Failed", wxICON_ERROR);
        auto* psp = static_cast<ProjectSelectPanel*>(book->GetPage(0));
        psp->FindWindowByLabel("Discover Features")->Enable();
        psp->FindWindowByLabel("Discover Features")->SetLabel("Discover Features");
    } else {
        book->AddPage(new FeatureSelectPanel(book, discoveredPackages), "Select Features");
        book->SetSelection(1);
    }
}

void MyFrame::OnResolveComplete(wxCommandEvent& event) {
    book->AddPage(new ImportPanel(book, this), "Import Features");
    progressPanel = new ProgressPanel(book);
    book->AddPage(progressPanel, "Progress");
    book->AddPage(new CompletionPanel(book, this), "Complete");
    book->SetSelection(2);
}

void MyFrame::StartInstallation() {
    std::thread worker_thread([this]() {
        auto* old_cout_buf = std::cout.rdbuf();
        std::stringstream new_cout_stream;
        std::cout.rdbuf(new_cout_stream.rdbuf());
        auto post_update = [&]() {
            std::string text = new_cout_stream.str();
            new_cout_stream.str("");
            if (!text.empty()) {
                wxThreadEvent* update_event = new wxThreadEvent(wxEVT_INSTALL_UPDATE);
                update_event->SetString(text);
                wxQueueEvent(this, update_event);
            }
        };
        tool.downloadAndRepackage();
        post_update();
        tool.installPackagesToProject(projectPath);
        post_update();
        std::cout.rdbuf(old_cout_buf);
        wxQueueEvent(this, new wxThreadEvent(wxEVT_INSTALL_COMPLETE));
    });
    worker_thread.detach();
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
    book->ChangeSelection(0);
}

// --- ProjectSelectPanel Implementation ---
ProjectSelectPanel::ProjectSelectPanel(wxWindow* parent) : wxPanel(parent) {
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
    for (auto child : this->GetChildren()) { if (auto* staticChild = dynamic_cast<wxStaticText*>(child)) { staticChild->SetForegroundColour(wxColour(220, 220, 220)); } }
    titleLabel->SetForegroundColour(*wxWHITE);
    browseButton->Bind(wxEVT_BUTTON, &ProjectSelectPanel::OnBrowse, this);
    pathTextCtrl->Bind(wxEVT_TEXT, &ProjectSelectPanel::OnPathChanged, this);
    discoverButton->Bind(wxEVT_BUTTON, &ProjectSelectPanel::OnDiscover, this);
}
void ProjectSelectPanel::OnBrowse(wxCommandEvent& event) { wxDirDialog d(this, "Select a Unity Project Folder", "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST); if (d.ShowModal() == wxID_OK) { pathTextCtrl->SetValue(d.GetPath()); } }
void ProjectSelectPanel::OnPathChanged(wxCommandEvent& event) { ValidateProjectPath(); }
void ProjectSelectPanel::ValidateProjectPath() { fs::path p(pathTextCtrl->GetValue().ToStdString()); MyFrame* mf = static_cast<MyFrame*>(GetParent()->GetParent()); if (MRTKToolCore::isValidUnityProject(p)) { versionText->SetLabel(MRTKToolCore::getUnityVersion(p)); versionText->SetForegroundColour(wxColour(150, 255, 150)); discoverButton->Enable(); mf->projectPath = p; } else { versionText->SetLabel("<Invalid Project Path>"); versionText->SetForegroundColour(wxColour(255, 150, 150)); discoverButton->Disable(); } Layout(); }
void ProjectSelectPanel::OnDiscover(wxCommandEvent& event) { discoverButton->SetLabel("Discovering..."); discoverButton->Disable(); MyFrame* mf = static_cast<MyFrame*>(GetParent()->GetParent()); std::thread t([mf]() { mf->tool.fetchAvailablePackages(); wxQueueEvent(mf, new wxThreadEvent(wxEVT_DISCOVERY_COMPLETE)); }); t.detach(); }


// --- FeatureSelectPanel Implementation ---
FeatureSelectPanel::FeatureSelectPanel(wxWindow* parent, const std::vector<SelectablePackage>& packages) : wxPanel(parent) {
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
        label->Bind(wxEVT_LEFT_DOWN, [cb](wxMouseEvent& event){ 
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
    for (int i = 0; i < checkBoxes.size(); ++i) {
        if (checkBoxes[i]->IsChecked()) selectedIndices.push_back(i);
    }
    if (selectedIndices.empty()) { wxMessageBox("No features were selected.", "Warning", wxOK | wxICON_WARNING); return; }
    
    MyFrame* mainFrame = static_cast<MyFrame*>(GetParent()->GetParent());
    std::thread worker_thread([mainFrame, selectedIndices]() {
        mainFrame->tool.resolveDependencies(selectedIndices);
        wxQueueEvent(mainFrame, new wxCommandEvent(wxEVT_RESOLVE_COMPLETE));
    });
    worker_thread.detach();
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