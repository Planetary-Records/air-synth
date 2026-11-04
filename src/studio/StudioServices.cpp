#include "StudioServices.h"

namespace airack::studio
{
namespace
{
/** Owns a chooser until its callback unwinds, or until the component that asked for it is deleted, and then deletes
    itself. Deleting the chooser closes its dialog without calling back. */
class PendingChooser final : private juce::ComponentListener
{
public:
    static void launch (const FileRequest& r, std::function<void (const juce::File&)> done)
    {
        (new PendingChooser (r, std::move (done)))->start (r.saving);
    }

private:
    PendingChooser (const FileRequest& r, std::function<void (const juce::File&)> d)
        : done (std::move (d)), parent (r.parent),
          chooser (std::make_unique<juce::FileChooser> (r.title, r.initial, r.patterns, r.osDialog))
    {
        if (parent != nullptr)
            parent->addComponentListener (this);
    }

    ~PendingChooser() override { detach(); }

    void start (bool saving)
    {
        const auto flags = (saving ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                                   : juce::FileBrowserComponent::openMode)
                         | juce::FileBrowserComponent::canSelectFiles;
        chooser->launchAsync (flags, [this] (const juce::FileChooser& c)
        {
            const auto file = c.getResult();
            const auto callback = std::move (done);
            detach();
            juce::MessageManager::callAsync ([this] { delete this; });
            if (callback)
                callback (file);
        });
    }

    void detach()
    {
        if (parent != nullptr)
            parent->removeComponentListener (this);
        parent = nullptr;
    }

    void componentBeingDeleted (juce::Component&) override
    {
        detach();
        delete this;
    }

    std::function<void (const juce::File&)> done;
    juce::Component* parent;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace

StudioServices defaultStudioServices()
{
    StudioServices s;
    s.chooseFile = [] (const FileRequest& r, std::function<void (const juce::File&)> done) { PendingChooser::launch (r, std::move (done)); };
    s.nowMs = [] { return (std::int64_t) juce::Time::currentTimeMillis(); };
    s.dragFiles = [] (const juce::StringArray& files, bool canMove, juce::Component* source, std::function<void()> done)
    { return juce::DragAndDropContainer::performExternalDragDropOfFiles (files, canMove, source, std::move (done)); };
    s.copyText = [] (const juce::String& text) { juce::SystemClipboard::copyTextToClipboard (text); };
    s.chooseMenu = [] (juce::Component* target, const juce::StringArray& items, std::function<void(int)> done)
    {
        juce::PopupMenu menu;
        for (int i=0;i<items.size();++i) menu.addItem(i+1,items[i]);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target),std::move(done));
    };
    return s;
}
} // namespace airack::studio
