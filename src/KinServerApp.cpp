#include "cinder/app/RendererGl.h"
#include "cinder/app/App.h"
#include "cinder/gl/gl.h"
#include "cinder/gl/scoped.h"
#include "cinder/Log.h"
#include "cinder/Json.h"
#include "cinder/Url.h"
#include "cinder/Utilities.h"

#include <vector>

#include "DepthSensor.h"
#include "Cinder-VNM/include/MiniConfigImgui.h"
#include "Cinder-VNM/include/AssetManager.h"
#include "Cinder-VNM/include/TextureHelper.h"
#include "Cinder-VNM/include/FontHelper.h"

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
    gl::Texture2dRef processTex;
    Channel16u depthChannel;
    Surface colorSurface;
    Channel8u processChannel;
    int itemUsedCount = 0;
    bool isItemUsing = false;

    void notifyHTTPStatus()
    {
        char urlName[256];
        sprintf(urlName, "http://%s:%d/api/objectitem/%s/%s",
            SERVER_ADDR.c_str(), SERVER_PORT,
            isItemUsing ? "pickup" : "return",
            name.c_str());
        auto url = loadUrl(Url(urlName));
        auto str = loadString(url);
        CI_LOG_I(urlName);
        CI_LOG_I(str);
    }

    void updateItemUsing(bool changeState)
    {
        if (isItemUsing)
        {
            if (changeState)
            {
                isItemUsing = false;
                notifyHTTPStatus();
            }
        }
        else
        {
            if (changeState)
            {
                isItemUsing = true;
                notifyHTTPStatus();
                itemUsedCount++;
            }
        }
    }

    JsonTree write()
    {
        auto depthPath = getAssetPath("") / "items" / (name + "_depth.hdr");
        auto colorPath = getAssetPath("") / "items" / (name + "_color.png");
        if (depthChannel.getWidth() > 0)
        {
            writeImage(depthPath, depthChannel);
            writeImage(colorPath, colorSurface);
        }

        JsonTree tree;
        tree.addChild(JsonTree("name", name));
        tree.addChild(JsonTree("depthPath", depthPath.string()));
        tree.addChild(JsonTree("colorPath", colorPath.string()));
        tree.addChild(JsonTree("pos_x", pos.x));
        tree.addChild(JsonTree("pos_y", pos.y));
        tree.addChild(JsonTree("size_x", size.x));
        tree.addChild(JsonTree("size_y", size.y));
        tree.addChild(JsonTree("itemUsedCount", itemUsedCount));

        return tree;
    }

    bool read(const JsonTree& tree)
    {
        name = tree.getValueForKey("name");
        string depthPath = tree.getValueForKey("depthPath");
        string colorPath = tree.getValueForKey("colorPath");
        pos.x = tree.getValueForKey<float>("pos_x");
        pos.y = tree.getValueForKey<float>("pos_y");
        size.x = tree.getValueForKey<float>("size_x");
        size.y = tree.getValueForKey<float>("size_y");
        itemUsedCount = tree.getValueForKey<int>("itemUsedCount");

        depthChannel = am::channel16u(depthPath)->clone();
        colorSurface = am::surface(colorPath)->clone();
        processChannel = { depthChannel.getWidth(), depthChannel.getHeight() };

        _createTex();

        return true;
    }

    Rectf getRect() const
    {
        return Rectf(pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    }

    void update(const Channel16u& depth, const Surface& color)
    {
        depthChannel = depth.clone(Area(getRect()), true);
        processChannel = { size.x, size.y };

        auto rect = getRect();
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
        updateTexture(depthTex, depthChannel, getTextureFormatUINT16());
        updateTexture(colorTex, colorSurface);
        updateTexture(processTex, processChannel);
    }
};

class AmazonGoApp : public App
{
public:

    vector<MonitorItem>	mItems;
    int selectedItem = -1;

    gl::TextureFontRef mFont;

    void setup() override
    {       
        const auto& args = getCommandLineArgs();
        log::makeLogger<log::LoggerFile>();

        CI_LOG_I(gl::getVendorString());
        CI_LOG_I(gl::getVersionString());

        mFont = FontHelper::createTextureFont();

        readConfig();
        createConfigImgui();

        ds::DeviceType type = ds::DeviceType(_SENSOR_TYPE);
        ds::Option option;
        option.enableColor = true;
        option.enableDepth = true;
        option.enablePointCloud = true;
        mDevice = ds::Device::create(type, option);
        if (!mDevice->isValid())
        {
            CI_LOG_F("Faile to create depth sensor: " << type);
            quit();
        }

        mDevice->signalColorDirty.connect([&] {
            updateTexture(mColorTexture, mDevice->colorSurface);
        });

        mDevice->signalDepthDirty.connect(std::bind(&AmazonGoApp::updateDepthRelated, this));

        mDevice->signalDepthToColorTableDirty.connect([&] {
            auto format = gl::Texture::Format()
                .dataType(GL_FLOAT)
                .immutableStorage();
            updateTexture(mDepthToColorTableTexture, mDevice->depthToColorTable, format);
        });

        getWindow()->setSize(_WINDOW_WIDTH, _WINDOW_HEIGHT);
        getWindow()->setPos(_WINDOW_X, _WINDOW_Y);
        getWindow()->setTitle("SmartMonitor");

        gl::enableAlphaBlending();
        gl::disableDepthRead();

        mDepthShader = am::glslProg("depthMap.vs", "depthMap.fs");
        mDepthShader->uniform("uDepthTexture", 0);
        mColorShader = am::glslProg("depthMap.vs", "colorMap.fs");
        mColorShader->uniform("uColorTexture", 0);
        mColorShader->uniform("uDepthToColorTableTexture", 1);

        onLoadItems();
    }

    void cleanup()
    {
        //onSaveItems();
        //writeConfig();
    }

    void resize() override
    {
        mLayout.width = getWindowWidth();
        mLayout.height = getWindowHeight();
        mLayout.halfW = mLayout.width / 2;
        mLayout.halfH = mLayout.height / 2;
        mLayout.spc = mLayout.width * 0.01;

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

#if DEMO_MODE == 1
        // HACK
        mLayout.canvases[0].offset({ DEPTH_ROI_X1 * getWindowWidth(), DEPTH_ROI_Y1 * getWindowHeight() });
        mLayout.canvases[2].offset({ DEPTH_ROI_X2 * getWindowWidth(), DEPTH_ROI_Y2 * getWindowHeight() });
#endif
    }

    void draw() override
    {
        gl::clear(ColorA::gray(0.3f));

        if (mDepthW == 0) return;

        int canvasIds[] = { 2, 0 };

        if (mColorTexture && mDepthToColorTableTexture)
        {
            gl::ScopedGlslProg prog(mColorShader);
            gl::ScopedTextureBind tex0(mColorTexture, 0);
            gl::ScopedTextureBind tex1(mDepthToColorTableTexture, 1);
            gl::drawSolidRect(mLayout.canvases[canvasIds[0]]);
            //gl::drawSolidRect(mLayout.canvases[canvasIds[1]], { DEPTH_ROI_X1, DEPTH_ROI_Y1 }, { DEPTH_ROI_X2, DEPTH_ROI_Y2 });
        }

        if (mDepthTexture)
        {
            gl::ScopedGlslProg prog(_DEPTH_AS_RGB ? am::glslProg("texture") : mDepthShader);
            gl::ScopedTextureBind tex0(mDepthTexture, 0);
            gl::drawSolidRect(mLayout.canvases[canvasIds[1]]);
            //gl::drawSolidRect(mLayout.canvases[canvasIds[0]], { DEPTH_ROI_X1, DEPTH_ROI_Y1 }, { DEPTH_ROI_X2, DEPTH_ROI_Y2 });
        }

        gl::ScopedColor scopedColor;
        for (int i : canvasIds)
        {
            vec2 scale;
            scale.x = (mLayout.halfW - mLayout.spc * 2) / mDepthW;
            scale.y = (mLayout.halfH - mLayout.spc * 2) / mDepthH;
            gl::ScopedModelMatrix model;
            gl::translate(mLayout.canvases[i].getUpperLeft());
            gl::scale(scale);

            int idx = 0;
            for (auto& item : mItems)
            {
                if (DEMO_MODE)
                {
                    if (i == 0)
                    {
                        gl::color(ColorA(1, 0, 0, 1));
                        mFont->drawString(toString(item.itemUsedCount), { item.pos.x, item.pos.y});
                    }
                }
                else
                {
                    if (idx == selectedItem)
                        gl::color(ColorA(1, 0, 0, 1));
                    else
                        gl::color(ColorA(0, 0, 0, 1));

                    gl::drawStrokedRoundedRect(item.getRect(), 5.0f);
                    mFont->drawString(i == 0 ? item.name : toString(item.itemUsedCount), { item.pos.x, item.pos.y - 5 });
                }
                idx++;
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

    void onLoadItems()
    {
        auto filename = getAssetPath("items.json");
        if (!fs::exists(filename)) return;

        mItems.clear();

        JsonTree itemsJson;
        try
        {
            itemsJson = JsonTree(loadFile(filename));
            for (const auto& itemJson : itemsJson)
            {
                MonitorItem item;
                if (!item.read(itemJson)) continue;

                mItems.emplace_back(item);
            }
        }
        catch (JsonTree::Exception& e)
        {
            CI_LOG_EXCEPTION("Loading Json", e);
        }
    }

    void onSaveItems()
    {
        auto itemsJsonPath = getAssetPath("") / "items.json";
        if (!mItems.empty())
        {
            JsonTree itemsJson;
            for (auto& item : mItems)
            {
                auto itemJson = item.write();
                itemsJson.addChild(itemJson);
            }
            itemsJson.write(itemsJsonPath);
        }
        else
        {
            writeString(itemsJsonPath, "{}");
        }
    }

    void update() override
    {
        _FPS = getAverageFps();

        if (MIN_DEPTH_FOR_VIZ_MM > MAX_DEPTH_FOR_VIZ_MM) MIN_DEPTH_FOR_VIZ_MM = MAX_DEPTH_FOR_VIZ_MM;

        mDepthShader->uniform("uFlipX", FLIP_X);
        mDepthShader->uniform("uFlipY", FLIP_Y);
        mDepthShader->uniform("uMinDepthForVizMM", MIN_DEPTH_FOR_VIZ_MM);
        mDepthShader->uniform("uMaxDepthForVizMM", MAX_DEPTH_FOR_VIZ_MM);

        mColorShader->uniform("uFlipX", FLIP_X);
        mColorShader->uniform("uFlipY", FLIP_Y);

        // create the main menu bar
        if (false)
        {
            ui::ScopedMainMenuBar menuBar;

            // add a file menu
            if (ui::BeginMenu("File"))
            {
                ui::MenuItem("Open");
                ui::MenuItem("Save");
                ui::MenuItem("Save As");
                ui::EndMenu();
            }
        }

        [&] {
            ui::ScopedWindow window("Config");
            ui::NewLine();
            if (!ui::CollapsingHeader("Items", ImGuiTreeNodeFlags_DefaultOpen)) return;

            if (ui::Button("Add"))
            {
                static int objCount = 0;

                MonitorItem item;
                item.pos = { 100, 100 };
                item.size = { 10, 10 };
                item.name = "item" + to_string(objCount++);
                item.update(mDevice->depthChannel, mDevice->colorSurface);

                mItems.emplace_back(item);
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

            if (ui::Button("Reload"))
            {
                onLoadItems();
            }

            ui::SameLine();
            if (ui::Button("Save"))
            {
                onSaveItems();
            }

            if (ui::Button("Refresh all"))
            {
                for (auto& item : mItems)
                {
                    item.update(mDevice->depthChannel, mDevice->colorSurface);
                    item.itemUsedCount = 0;
                    item.isItemUsing = false;
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
        }();

        if (selectedItem != -1)
        {
            ui::ScopedWindow window("Item");
            MonitorItem& item = mItems[selectedItem];
            ui::InputText("name", &item.name);
            ui::Text(item.isItemUsing ? "being used" : "still there");
            ui::DragInt("used count", &item.itemUsedCount);

            bool posXChanged = ui::DragInt("x", &item.pos.x, 1, 0, mDepthW - item.size.x);
            bool posYChanged = ui::DragInt("y", &item.pos.y, 1, 0, mDepthH - item.size.y);
            bool sizeXChanged = ui::DragInt("width", &item.size.x, 1, 0, mDepthW - item.pos.x);
            bool sizeYChanged = ui::DragInt("height", &item.size.y, 1, 0, mDepthH - item.pos.y);
            if (posXChanged || posYChanged || sizeXChanged || sizeYChanged)
            {
                item.update(mDevice->depthChannel, mDevice->colorSurface);
            }

            if (item.colorTex && item.depthTex)
            {
                auto size = item.depthTex->getSize();
                ui::NewLine();
                ui::Image(item.depthTex, size);

                ui::NewLine();
                ui::Image(item.colorTex, size);

                ui::NewLine();
                ui::Image(item.processTex, size);
            }
        }
    }

private:

    void updateDepthRelated()
    {
        if (mDepthW == 0)
        {
            mDepthW = mDevice->getDepthSize().x;
            mDepthH = mDevice->getDepthSize().y;
        }
        
        if (!_DEPTH_AS_RGB)
        {
            updateTexture(mDepthTexture, mDevice->depthChannel, getTextureFormatUINT16());
        }
        else
        {
            if (mDepthAsColorSurface.getWidth() == 0)
            {
                mDepthAsColorSurface = Surface(mDepthW, mDepthH, false, SurfaceChannelOrder::RGB);
            }
            
            for (int y = 0; y < mDepthH; y++)
            {
                for (int x = 0; x < mDepthW; x++)
                {
                    uint16_t* src = mDevice->depthChannel.getData({x,y});
                    float t = math<uint16_t>::clamp(*src, 0, 4000) / 4000.0f;
                    uint8_t* dst = mDepthAsColorSurface.getData({ x, y });
                    
                    // https://twitter.com/Donzanoid/status/903424376707657730
                    vec3 r = vec3(t) * 2.1f - vec3(1.8f, 1.14f, 0.3f);
                    r = 1.0f - r * r;

                    dst[0] = r.x * 255;
                    dst[1] = r.y * 255;
                    dst[2] = r.z * 255;
                }
            }
            updateTexture(mDepthTexture, mDepthAsColorSurface);
        }
        gl::checkError();

        float depthToMmScale = mDevice->getDepthToMmScale();
        float minThresholdInDepthUnit = ITEM_HEIGHT_MM / depthToMmScale;
        float minThresholdBackInDepthUnit = ITEM_RETURN_ABSOLUTE_HEIGHT_MM / depthToMmScale;

        for (auto& item : mItems)
        {
            int pixelCountThreshold = 0;
            if (item.isItemUsing)
                pixelCountThreshold = item.size.x * item.size.y * ITEM_USING_RATIO;
            else
                pixelCountThreshold = item.size.x * item.size.y * ITEM_RETURN_RATIO;

            int count = 0;
            for (int j = 0; j < item.size.y; j++)
            {
                int y = j;
                for (int i = 0; i < item.size.x; i++)
                {
                    auto bg = *item.depthChannel.getData(i, j);
                    auto dep = *mDevice->depthChannel.getData(item.pos.x + i, item.pos.y + j);

                    // TODO: refactor
                    if (item.isItemUsing)
                    {
                        auto diff = abs(dep - bg);
                        if (dep > 0 && diff < minThresholdBackInDepthUnit)
                        {
                            *item.processChannel.getData(i, j) = diff & 0xff;
                            count++;
                        }
                        else
                        {
                            *item.processChannel.getData(i, j) = 0;
                        }
                    }
                    else
                    {
                        auto diff = dep - bg;
                        if (dep > 0 && diff > minThresholdInDepthUnit)
                        {
                            *item.processChannel.getData(i, j) = diff & 0xff;
                            count++;
                        }
                        else
                        {
                            *item.processChannel.getData(i, j) = 0;
                        }

                    }
                }
            }
            item.updateItemUsing(count > pixelCountThreshold);
            updateTexture(item.processTex, item.processChannel);
        }
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
    int mDepthW = 0, mDepthH = 0;

    gl::TextureRef mDepthTexture;
    gl::TextureRef mColorTexture;
    gl::TextureRef mDepthToColorTableTexture;

    Surface mDepthAsColorSurface;

    gl::GlslProgRef	mDepthShader, mColorShader;
};

CINDER_APP(AmazonGoApp, RendererGl)
