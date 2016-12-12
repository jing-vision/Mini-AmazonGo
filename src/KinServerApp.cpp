#include "cinder/app/RendererGl.h"
#include "cinder/app/App.h"
#include "cinder/gl/gl.h"
#include "cinder/gl/scoped.h"
#include "cinder/Log.h"
#include "cinder/Json.h"

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
    ivec2   pos;
    ivec2   size;
    gl::Texture2dRef depthTex;
    gl::Texture2dRef colorTex;
    Channel16u depthChannel;
    Surface colorSurface;

    Rectf getRectfInDepth() const
    {
        return Rectf(pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    }

    Rectf getRectfInColor() const
    {
        return Rectf(pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    }

    void grab(const Channel16u& depth, const Surface& color)
    {
        depthChannel = depth.clone(Area(getRectfInDepth()), true);
        auto rect = getRectfInColor();
        float xScale = color.getWidth() / (float)depth.getWidth();
        float yScale = color.getHeight() / (float)depth.getHeight();
        rect.x1 *= xScale;
        rect.x2 *= xScale;
        rect.y1 *= yScale;
        rect.y2 *= yScale;
        colorSurface = color.clone(Area(rect), true);
        _createTex();
    }

    void _createTex()
    {
        if (depthChannel.getWidth() > 0)
            depthTex = gl::Texture2d::create(depthChannel);
        if (colorSurface.getWidth() > 0)
            colorTex = gl::Texture2d::create(colorSurface);
    }

    void load()
    {
        depthChannel = *am::channel16u(name + "_depth.png");
        colorSurface = *am::surface(name + "_color.png");
        _createTex();
    }

    void save()
    {
        if (depthChannel.getWidth() > 0)
        {
            writeImage(getAssetPath("") / (name + "_depth.png"), depthChannel);
            writeImage(getAssetPath("") / (name + "_color.png"), colorSurface);
        }
    }
};

class KinServerApp : public App
{
public:

    vector<MonitorItem>	mItems;
    int selectedItem = -1;

    void setup() override
    {
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
        mColorW = mDevice->getColorSize().x;
        mColorH = mDevice->getColorSize().y;

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

        int canvasIds[] = { 0, 1 };
        for (int i : canvasIds)
        {
            vec2 scale;
            scale.x = (mLayout.halfW - mLayout.spc * 2) / mDepthW;
            scale.y = (mLayout.halfH - mLayout.spc * 2) / mDepthH;
            gl::pushModelMatrix();
            gl::ScopedModelMatrix model;
            gl::translate(mLayout.canvases[i].getUpperLeft());
            gl::scale(scale);

            int idx = 0;
            for (auto& item : mItems)
            {
                if (idx == selectedItem)
                    gl::color(ColorA(1, 0, 0, 0.5f));

                gl::drawStrokedRoundedRect(item.getRectfInColor(), 5.0f);
                gl::drawStringCentered(item.name, vec2(item.pos), ColorA::white());

                idx++;

                gl::color(ColorA::white());
            }
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

    void _load()
    {
        auto content = am::str("items.txt");
        if (content.empty()) return;

        mItems.clear();

        auto itemNames = split(content, ',');
        for (auto& name : itemNames)
        {
            if (name.empty()) continue;

            MonitorItem item;
            item.name = name;
            item.load();

            mItems.emplace_back(item);
        }
    }

    void update() override
    {
        mFps = getAverageFps();

        {
            ui::ScopedWindow window("Items");
            if (ui::Button("Add"))
            {
                static int objCount = mItems.size();

                MonitorItem item;
                item.pos = { 100, 100 };
                item.size = { 50, 50 };
                item.name = "item" + to_string(objCount++);
                item.grab(mDevice->depthChannel, mDevice->colorSurface);

                mItems.push_back(item);
            }
            if (selectedItem != -1)
            {
                ui::SameLine();
                if (ui::Button("Remove"))
                {
                    mItems.erase(mItems.begin() + selectedItem);
                    selectedItem = -1;
                }
            }

            if (ui::Button("Load"))
            {
                _load();
            }

            ui::SameLine();
            if (ui::Button("Save"))
            {
                string filename = (getAssetPath("") / "items.json").string();
                ofstream ofs(filename);
                if (ofs)
                {
                    for (auto& item : mItems)
                    {
                        ofs << item.name << ",";
                        item.save();
                    }
                }
            }

            // selectable list
            ui::ListBoxHeader("");
            int idx = 0;
            for (auto& item : mItems)
            {
                if (ui::Selectable(item.name.c_str(), idx == selectedItem))
                {
                    selectedItem = idx;
                }
                idx++;
            }
            ui::ListBoxFooter();
        }

        // The item Inspector
        if (selectedItem != -1)
        {
            ui::ScopedWindow window("Inspector");

            MonitorItem& item = mItems[selectedItem];
            ui::InputText("name", &item.name);
            if (ui::Button("grab"))
            {
                item.grab(mDevice->depthChannel, mDevice->colorSurface);
            }

            // getter/setters are a bit longer but still possible
            ui::DragInt2("pos", &item.pos.x);
            ui::DragInt2("size", &item.size.x);

            if (item.colorTex && item.depthTex)
            {
                auto size = item.colorTex->getSize();
                ui::NewLine();
                ui::Image(item.depthTex, size);

                ui::NewLine();
                ui::Image(item.colorTex, size);
            }
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
    int mColorW, mColorH;

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
