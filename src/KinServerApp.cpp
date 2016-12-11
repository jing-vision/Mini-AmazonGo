#include "cinder/app/RendererGl.h"
#include "cinder/app/App.h"
#include "cinder/gl/gl.h"
#include "cinder/gl/scoped.h"
#include "cinder/Log.h"
#include <vector>

#include "DepthSensor.h"
#include "Cinder-VNM/include/MiniConfigImgui.h"
#include "Cinder-VNM/include/AssetManager.h"
#include "Cinder-VNM/include/TextureHelper.h"

#include "CinderImGui.h"

using namespace ci;
using namespace ci::app;
using namespace std;

struct MonitorItem
{
    string	name;
    Area   bbox;
    gl::Texture2dRef tex;

    void load()
    {

    }

    void save()
    {

    }
};

static const MonitorItem* selection;

class KinServerApp : public App
{
public:

    vector<MonitorItem>	mItems;
    int selectedItem = 0;

    void setup() override
    {
        mItems = {
            { "Item0", { 10, 10, 100, 100 }, nullptr },
        };
        const auto& args = getCommandLineArgs();
        readConfig();
        log::makeLogger<log::LoggerFile>();

        createConfigImgui();
        //mParams->addParam("FPS", &mFps, true);
        //mParams->addButton("Set Bg", std::bind(&KinServerApp::updateBack, this));

        ds::DeviceType type = ds::DeviceType(_SENSOR_TYPE);
        ds::Option option;
        option.enableColor = true;
        option.enableDepth = true;
        mDevice = ds::Device::create(type, option);
        if (!mDevice->isValid())
        {
            quit();
            return;
        }

        mDirtyConnection = mDevice->signalColorDirty.connect(std::bind(&KinServerApp::updateColorRelated, this));
        mDirtyConnection = mDevice->signalDepthDirty.connect(std::bind(&KinServerApp::updateDepthRelated, this));

        mDepthW = mDevice->getDepthSize().x;
        mDepthH = mDevice->getDepthSize().y;
        mDiffChannel = Channel8u(mDepthW, mDepthH);

        getWindow()->setSize(1024, 768);
        getWindow()->setTitle("SmartMonitor");
        
        gl::disableAlphaBlending();

        mShader = am::glslProg("depthMap.vs", "depthMap.fs");
    }

    void resize() override
    {
        mLayout.width = getWindowWidth();
        mLayout.height = getWindowHeight();
        mLayout.halfW = mLayout.width / 2;
        mLayout.halfH = mLayout.height / 2;
        mLayout.spc = mLayout.width * 0.04;

        for (int x = 0; x < 2; x++)
        {
            for (int y = 0; y < 2; y++)
            {
                mLayout.canvases[y * 2 + x] = Rectf(
                    mLayout.spc + mLayout.halfW * x,
                    mLayout.spc + mLayout.halfH * y,
                    mLayout.halfW * (1 + x) - mLayout.spc,
                    mLayout.halfH * (1 + y) - mLayout.spc
                    );
            }
        }
    }

    void draw() override
    {
        gl::clear(ColorA::gray(0.5f));

        if (mDepthTexture)
        {
            gl::ScopedGlslProg prog(mShader);
            gl::ScopedTextureBind tex0(mDepthTexture);
            gl::drawSolidRect(mLayout.canvases[0]);
            gl::ScopedTextureBind tex1(mBackTexture);
            gl::drawSolidRect(mLayout.canvases[3]);
            gl::ScopedTextureBind tex2(mDiffTexture);
            gl::drawSolidRect(mLayout.canvases[2]);
        }
        gl::draw(mColorTexture, mLayout.canvases[1]);

        int idx = 0;
        for (auto object : mItems)
        {
            if (idx = selectedItem)
                gl::color(ColorA(1, 0, 0, 0.5f));

            gl::drawStrokedRoundedRect(object.bbox, 0.1f);
            gl::drawStringCentered(object.name, vec2(object.bbox.getUL()) - vec2(0, 5), ColorA::black());

            idx++;

            gl::color(ColorA::white());
        }
    }

    void keyUp(KeyEvent event) override
    {
        int code = event.getCode();
        if (code == KeyEvent::KEY_ESCAPE)
        {
            quit();
        }
    }

    void update() override
    {
        mFps = getAverageFps();

        selection = &mItems[selectedItem];

        {
            ui::ScopedWindow window("Items");
            if (ui::Button("Add"))
            {
                static int objCount = mItems.size();

                MonitorItem item;
                item.bbox = { 100, 100, 150, 150 };
                item.name = "item" + to_string(objCount++);
                mItems.push_back(item);
                selection = &mItems[selectedItem];
            }
            if (selection) {
                ui::SameLine();
                if (ui::Button("Remove")) {
                    auto it = std::find_if(mItems.begin(), mItems.end(), [](const MonitorItem& obj) { return &obj == selection; });
                    if (it != mItems.end()) {
                        mItems.erase(it);
                        selection = nullptr;
                    }
                }
            }

            // selectable list
            ui::ListBoxHeader("");
            int idx = 0;
            for (const auto& object : mItems)
            {
                if (ui::Selectable(object.name.c_str(), selection == &object))
                {
                    selection = &object;
                    selectedItem = idx;
                }
                idx++;
            }
            ui::ListBoxFooter();
        }

        // The Object Inspector
        if (selection) {
            ui::ScopedWindow window("Inspector");

            MonitorItem* object = (MonitorItem*)selection;
            ui::InputText("name", &object->name);
            // getter/setters are a bit longer but still possible
            int32_t* ptr = &object->bbox.x1;
            ui::DragInt4("bbox", ptr);
        }

    }

private:

    void updateColorRelated()
    {
        updateTexture(mColorTexture, mDevice->colorSurface);
    }
        
    void updateDepthRelated()
    {
        updateTexture(mDepthTexture, mDevice->depthChannel);

        if (!mBackTexture)
        {
            updateBack();
        }

        //mDiffMat.setTo(cv::Scalar::all(0));

        float depthToMmScale = mDevice->getDepthToMmScale();
        float minThresholdInDepthUnit = MIN_THRESHOLD_MM / depthToMmScale;
        float maxThresholdInDepthUnit = MAX_THRESHOLD_MM / depthToMmScale;

        for (int yy = 0; yy < mDepthH; yy++)
        {
            // TODO: cache row pointer
            int y = yy;
            for (int xx = 0; xx < mDepthW; xx++)
            {
                int x = LEFT_RIGHT_FLIPPED ? (mDepthW - xx) : xx;
                auto bg = *mBackChannel.getData(x, y);
                auto dep = *mDevice->depthChannel.getData(x, y);
                auto diff = dep - bg;
                if (dep > 0 && diff > minThresholdInDepthUnit && diff < maxThresholdInDepthUnit)
                {
                    //mDiffMat(yy, xx) = 255;
                }
            }
        }

        updateTexture(mDiffTexture, mDiffChannel);
    }

    void updateBack()
    {
        mBackChannel = mDevice->depthChannel.clone();
        updateTexture(mBackTexture, mBackChannel);
    }

    float mFps = 0;

    struct Layout
    {
        float width;
        float height;
        float halfW;
        float halfH;
        float spc;

        Rectf canvases[4];
        Rectf logoRect;
    } mLayout;

    ds::DeviceRef mDevice;
    signals::Connection mDirtyConnection;
    int mDepthW, mDepthH;

    gl::TextureRef mDepthTexture;
    gl::TextureRef mColorTexture;

    // vision
    Channel16u mBackChannel;
    gl::TextureRef mBackTexture;

    Channel8u mDiffChannel;
    gl::TextureRef mDiffTexture;

    gl::GlslProgRef	mShader;
};

CINDER_APP(KinServerApp, RendererGl)
