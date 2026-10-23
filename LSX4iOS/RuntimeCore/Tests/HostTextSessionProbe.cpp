// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone headless test of the production session queue, ImGui event routing
// and shared guest IME dialog. It does not initialize a Vulkan device.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <imgui.h>
#include <imgui_internal.h>
#include "imgui/host_text_session.h"
#include "imgui/renderer/imgui_core.h"
#include "core/libraries/ime/ime_dialog_ui.h"
#include "common/elf_info.h"
#include "core/aerolib/stubs.h"

extern "C" std::uint64_t executor_lsx4_runtime_system_ui_text_session();
extern "C" int executor_lsx4_runtime_system_ui_text_session_event(
    std::uint64_t,unsigned,const char*,std::size_t,unsigned);
extern "C" int executor_lsx4_runtime_system_ui_text_input(const char*);

namespace {
using Queue=ImGui::Core::HostTextSessionQueue;
using Action=Queue::Action;
using namespace Libraries::ImeDialog;
void Require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
bool Send(std::uint64_t session,Action action,std::string_view text={},unsigned count=1) {
    return executor_lsx4_runtime_system_ui_text_session_event(
        session,static_cast<unsigned>(action),text.data(),text.size(),count)==1;
}
void Frames(unsigned count=1) {
    for(unsigned i=0;i<count;++i) {ImGui::Core::NewFrame();ImGui::Render();}
}
struct Dialog {
    std::vector<char16_t> guarded;
    OrbisImeDialogParam param{};
    OrbisImeDialogStatus status=OrbisImeDialogStatus::Running;
    OrbisImeDialogResult result{};
    std::unique_ptr<ImeDialogState> state;
    std::unique_ptr<ImeDialogUi> ui;
    explicit Dialog(unsigned maximum=64,bool numeric=false,OrbisImeTextFilter filter=nullptr,bool multiline=false,std::u16string_view initial={}) : guarded(maximum+3,0) {
        guarded.front()=0xcafe;guarded.back()=0xbeef;
        param.user_id=1;param.max_text_length=maximum;param.input_text_buffer=guarded.data()+1;
        param.type=numeric?OrbisImeType::Number:OrbisImeType::Default;
        param.filter=filter;
        Require(initial.size()<=maximum,"initial IME text fits guest buffer");
        std::copy(initial.begin(),initial.end(),guarded.begin()+1);
        if(multiline)param.option=OrbisImeOption::MULTILINE;
        state=std::make_unique<ImeDialogState>(&param,nullptr);
        ui=std::make_unique<ImeDialogUi>(state.get(),&status,&result);
    }
    ~Dialog() {ui.reset();}
    std::u16string Text() {
        Require(state->CopyTextToOrbisBuffer(),"copy UTF16 guest output");
        Require(guarded.front()==0xcafe && guarded.back()==0xbeef,"guest UTF16 buffer canaries");
        Require(guarded[param.max_text_length+1]==0,"guest UTF16 bounded termination");
        return std::u16string(param.input_text_buffer);
    }
    std::uint64_t Focus() {
        Frames(3);auto token=executor_lsx4_runtime_system_ui_text_session();
        Require(token!=0,"real IME field becomes session-active");return token;
    }
};
void Close(std::unique_ptr<Dialog>& dialog) {
    dialog.reset();Frames(3);
    Require(executor_lsx4_runtime_system_ui_text_session()==0,"removed dialog is inactive");
}
void QueueContracts() {
    Queue queue;
    Require(!queue.Current() && !queue.Queue(0,Action::Text,"x"),"no-session input rejected");
    queue.Focus(1);const auto first=queue.Current();Require(first!=0,"queue focus activates");
    queue.Focus(1);Require(queue.Current()==first,"same focus preserves token");
    std::string owned="copy";Require(queue.Queue(first,Action::Text,owned),"queue owned text");
    owned="changed";unsigned deliveries=0;
    queue.Drain([&](const Queue::Event& event){Require(event.text=="copy","queue owns UTF8 bytes");++deliveries;});
    Require(deliveries==1,"queued text delivered exactly once");
    for(const std::string malformed : {std::string("\xc0\xaf",2),std::string("\xed\xa0\x80",3),
            std::string("\xf4\x90\x80\x80",4),std::string("\xe2\x82",2),std::string("a\0b",3)})
        Require(!queue.Queue(first,Action::Text,malformed),"invalid UTF8 rejected");
    Require(!queue.Queue(first,Action::Text,""),"empty text rejected");
    Require(!queue.Queue(first,static_cast<Action>(99)),"invalid action rejected");
    Require(!queue.Queue(first,Action::Backspace,{},0),"zero backspace rejected");
    Require(!queue.Queue(first,Action::Submit,{},2),"repeated submit rejected");
    Require(!queue.Queue(first,Action::Text,std::string(Queue::MaxTextBytes+1,'x')),"text byte limit");
    for(unsigned i=0;i<Queue::MaxEvents;++i)Require(queue.Queue(first,Action::Text,"x"),"queue event bound accepts limit");
    Require(!queue.Queue(first,Action::Text,"x"),"queue event bound rejects excess");
    queue.Invalidate();Require(!queue.Current() && !queue.Queue(first,Action::Text,"old"),"invalidate rejects old token");
    queue.Focus(1);const auto second=queue.Current();Require(second!=first,"same field in next lifetime changes token");
    std::string maximum(Queue::MaxTextBytes,'x');
    for(unsigned i=0;i<4;++i)Require(queue.Queue(second,Action::Text,maximum),"queue cost limit accepts");
    Require(!queue.Queue(second,Action::Text,"x"),"queue cost bound rejects excess");
    queue.Focus(2);Require(queue.Current()!=second,"focused field changes token");
    deliveries=0;queue.Drain([&](const Queue::Event&){++deliveries;});Require(!deliveries,"focus change drops pending input");
    queue.Focus(0);Require(!queue.Current(),"focus loss invalidates");
}
static unsigned filter_calls;
int PS4_SYSV_ABI UppercaseFilter(char16_t* output,u32* output_length,const char16_t* input,u32 input_length) {
    Require(input_length==3 && std::u16string(input)==u"abc","shared text filter receives guest UTF16");
    Require(*output_length>=3,"text filter output capacity");
    output[0]=u'A';output[1]=u'B';output[2]=u'C';*output_length=3;++filter_calls;return 0;
}
void RealDialogContracts() {
    Require(!executor_lsx4_runtime_system_ui_text_session(),"no modal initially");
    Require(!Send(1,Action::Text,"x"),"public API rejects inactive input");
    auto dialog=std::make_unique<Dialog>();auto token=dialog->Focus();
    Require(Send(token,Action::Text,"Player \xd0\x91\xe6\xbc\xa2"),"BMP name accepted");Frames(3);
    Require(dialog->Text()==u"Player \u0411\u6f22","actual UTF8 to guest BMP UTF16");
    Require(Send(token,Action::Backspace),"backspace accepted");Frames(4);
    Require(dialog->Text()==u"Player \u0411","actual backspace removes last codepoint");
    Require(Send(token,Action::Text,"!"),"name suffix accepted");Frames(3);
    Require(dialog->Text()==u"Player \u0411!","text after backspace");
    Require(Send(token,Action::Submit),"submit accepted");Frames(3);
    Require(dialog->status==OrbisImeDialogStatus::Finished && dialog->result.endstatus==OrbisImeDialogEndStatus::Ok,
        "actual guest dialog submit result");Close(dialog);

    dialog=std::make_unique<Dialog>(8,true);token=dialog->Focus();
    Require(Send(token,Action::Text,"1a2,3.4"),"numeric input queued");Frames(3);
    Require(dialog->Text()==u"12,3.4","actual common numeric character filter");
    Require(Send(token,Action::Cancel),"cancel accepted");Frames(3);
    Require(dialog->status==OrbisImeDialogStatus::Finished && dialog->result.endstatus==OrbisImeDialogEndStatus::UserCanceled,
        "actual guest dialog cancel result");Close(dialog);

    dialog=std::make_unique<Dialog>(4);token=dialog->Focus();
    Require(Send(token,Action::Text,"ABCDE"),"overlength guest text accepted for shared clipping");Frames(3);
    Require(dialog->Text()==u"ABCD","shared guest maximum clips with canaries");Close(dialog);

    dialog=std::make_unique<Dialog>(8,false,&UppercaseFilter);token=dialog->Focus();
    Require(Send(token,Action::Text,"abc"),"filter input queued");Frames(3);
    Require(dialog->state->CallTextFilter() && filter_calls==1,"shared text-filter callback invoked");
    Require(dialog->Text()==u"ABC","shared text-filter result reaches guest UTF16");Close(dialog);
}
struct FocusFields final : ImGui::Layer {
    char first[32]{},second[32]{};
    int requested=0;
    FocusFields(){AddLayer(this);}
    ~FocusFields(){RemoveLayer(this);}
    void Draw() override {
        ImGui::Begin("Session focus fixture");
        if(requested==0)ImGui::SetKeyboardFocusHere();
        ImGui::InputText("First",first,sizeof(first));
        if(requested==1)ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Second",second,sizeof(second));
        ImGui::End();requested=-1;
    }
};
void RealFocusChange() {
    {
        FocusFields fields;Frames(3);
        const auto first=executor_lsx4_runtime_system_ui_text_session();Require(first!=0,"first real field focused");
        fields.requested=1;Frames(3);
        const auto second=executor_lsx4_runtime_system_ui_text_session();Require(second && first!=second,"real focus change produces new token");
        Require(!Send(first,Action::Text,"old") && Send(second,Action::Text,"new"),"focus token routes only to current field");
        Frames(3);Require(std::string(fields.first).empty() && std::string(fields.second)=="new","actual ImGui focus ownership");
    }
    Frames(3);
}
void SemanticMultilineSubmit() {
    auto dialog=std::make_unique<Dialog>(64,false,nullptr,true);auto token=dialog->Focus();
    Require(Send(token,Action::Text,"Player"),"multiline text queued");
    Require(Send(token,Action::Submit),"semantic multiline submit queued after text");
    Frames(8);
    Require(dialog->Text()==u"Player","submit must not insert newline into multiline guest text");
    Require(dialog->status==OrbisImeDialogStatus::Finished && dialog->result.endstatus==OrbisImeDialogEndStatus::Ok,
        "semantic submit confirms multiline guest dialog");Close(dialog);

    dialog=std::make_unique<Dialog>(64,false,nullptr,true);token=dialog->Focus();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);Frames(4);
    Require(dialog->status==OrbisImeDialogStatus::Running && dialog->Text()==u"\n",
        "ordinary Enter retains multiline editing semantics");
    Require(Send(token,Action::Cancel),"multiline cancel queued");Frames(4);
    Require(dialog->result.endstatus==OrbisImeDialogEndStatus::UserCanceled,"multiline cancel unchanged");Close(dialog);
}
void PrefilledMultilineSubmit() {
    auto dialog=std::make_unique<Dialog>(10,false,nullptr,true,u"Barista");
    const auto token=dialog->Focus();
    Require(Send(token,Action::Text,"Player"),"prefilled text queued");Frames(4);
    Require(Send(token,Action::Submit),"prefilled submit queued");Frames(4);
    Require(dialog->status==OrbisImeDialogStatus::Finished,
        "prefilled multiline field submits with gamepad backend present");Close(dialog);
}
void SubmitWithUnrelatedInput() {
    auto dialog=std::make_unique<Dialog>(64,false,nullptr,true);const auto token=dialog->Focus();
    Require(Send(token,Action::Text,"Player"),"continuous-input text queued");
    Require(Send(token,Action::Submit),"continuous-input submit queued");
    for(unsigned i=0;i<12;++i) {
        // A renderer/controller can publish pointer updates on every frame.
        // Those unrelated events must not starve a completed text session.
        ImGui::GetIO().AddMousePosEvent(20.f+i,20.f);Frames();
    }
    Require(dialog->Text()==u"Player","text stays ordered under continuous unrelated input");
    Require(dialog->status==OrbisImeDialogStatus::Finished,
        "unrelated pointer input must not starve semantic submit");Close(dialog);
}
void StaleBeforeDrain() {
    auto first=std::make_unique<Dialog>();const auto old=first->Focus();
    Require(Send(old,Action::Text,"stale-before"),"old event accepted before close");
    first.reset();auto second=std::make_unique<Dialog>();const auto fresh=second->Focus();
    Require(old!=fresh && !Send(old,Action::Text,"late"),"replacement rejects delayed token");
    Require(second->Text().empty(),"replacement receives no event queued before prior close");
    Require(Send(fresh,Action::Text,"new"),"replacement accepts new token");Frames(3);
    Require(second->Text()==u"new","replacement owns current input");Close(second);
}
void StaleAfterDrain() {
    auto first=std::make_unique<Dialog>();const auto old=first->Focus();
    Require(Send(old,Action::Backspace,{},4),"queue repeated old key");
    Require(Send(old,Action::Text,"stale-after"),"queue old text after repeated key");
    Require(Send(old,Action::Submit),"queue old submit");
    Frames(1);
    Require(ImGui::GetCurrentContext()->InputEventsQueue.Size>0,"actual ImGui trickle backlog exists");
    first.reset();auto second=std::make_unique<Dialog>();
    // This pre-existing route must survive pruning of unrelated stale sessions.
    Require(executor_lsx4_runtime_system_ui_text_input("legacy")==1,"legacy text route still accepts");
    Frames(12);
    Require(second->status==OrbisImeDialogStatus::Running,"stale submit cannot finish replacement");
    Require(second->Text()==u"legacy","stale session events removed while legacy event survives");
    Require(!Send(old,Action::Cancel),"delayed old cancel rejected");
    const auto fresh=executor_lsx4_runtime_system_ui_text_session();Require(fresh && fresh!=old,"replacement focus token");
    Require(Send(fresh,Action::Text,"!"),"fresh replacement text accepted");Frames(3);
    Require(second->Text()==u"legacy!","replacement key state released after stale keys");Close(second);
}
}
int main() {
    int exit_code=1;
    try {
        // The shared text-filter dispatcher first classifies callback addresses
        // through Core::Memory. Match its normal initialization prerequisites;
        // the fixture callback itself is native host code and takes the existing
        // direct-callback path, not guest JIT execution.
        Common::ElfInfo::Instance().InitializeForAndroidProbe(".",Common::ElfInfo::FW_45);
        Require(Core::AeroLib::ExecutorConfigureLibcAllocatorOwner(false),"callback address-classifier allocator owner");
        QueueContracts();
        ImGui::CreateContext();auto& io=ImGui::GetIO();
        io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad|ImGuiConfigFlags_DockingEnable;
        io.BackendFlags|=ImGuiBackendFlags_HasGamepad;
        io.DisplaySize=ImVec2(1280,720);io.DeltaTime=1.f/60.f;io.IniFilename=nullptr;io.LogFilename=nullptr;
        io.Fonts->AddFontDefault();Require(io.Fonts->Build(),"headless font atlas");
        RealDialogContracts();RealFocusChange();StaleBeforeDrain();StaleAfterDrain();SemanticMultilineSubmit();SubmitWithUnrelatedInput();PrefilledMultilineSubmit();
        ImGui::DestroyContext();
        std::fprintf(stderr,"HOST_TEXT_SESSION_TEST_PASS queue=1 utf8=1 bmpUtf16=1 backspace=1 submit=1 cancel=1 numeric=1 textFilter=1 guestClipCanaries=1 realFocus=1 staleBeforeDrain=1 staleAfterDrain=1 legacyPreserved=1 multilineSubmit=1 ordinaryMultilineEnter=1\n");
        exit_code=0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"HOST_TEXT_SESSION_TEST_FAIL %s\n",error.what());
    }
    std::fflush(nullptr);std::_Exit(exit_code);
}
