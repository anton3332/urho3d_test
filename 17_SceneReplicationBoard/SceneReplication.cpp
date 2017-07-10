//
// Copyright (c) 2008-2016 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/Graphics/Camera.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Graphics/Light.h>
#include <Urho3D/Graphics/Material.h>
#include <Urho3D/Graphics/Model.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/Graphics/Renderer.h>
#include <Urho3D/Graphics/StaticModel.h>
#include <Urho3D/Graphics/Zone.h>
#include <Urho3D/Input/Controls.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Network/Connection.h>
#include <Urho3D/Network/Network.h>
#include <Urho3D/Network/NetworkEvents.h>
#include <Urho3D/Physics/CollisionShape.h>
#include <Urho3D/Physics/PhysicsEvents.h>
#include <Urho3D/Physics/PhysicsWorld.h>
#include <Urho3D/Physics/RigidBody.h>
#include <Urho3D/Resource/ResourceCache.h>
#include <Urho3D/Scene/Scene.h>
#include <Urho3D/UI/Button.h>
#include <Urho3D/UI/Font.h>
#include <Urho3D/UI/LineEdit.h>
#include <Urho3D/UI/Text.h>
#include <Urho3D/UI/UI.h>
#include <Urho3D/UI/UIEvents.h>

#include "SceneReplication.h"

#include <Urho3D/DebugNew.h>

// UDP port we will use
static const unsigned short SERVER_PORT = 2346;

// refresh only one point (incremental update)
URHO3D_EVENT(E_REFRESH_POINT, RefreshPoint)
{
	URHO3D_PARAM(P_POS, Pos);		// IntVector2
	URHO3D_PARAM(P_COLOR, Color);	// Color
}

// refresh full texture (full update)
URHO3D_EVENT(E_REFRESH_TEXTURE, RefreshTexture)
{
	URHO3D_PARAM(P_DATA, Data);		// buffer
}

URHO3D_DEFINE_APPLICATION_MAIN(SceneReplication)

SceneReplication::SceneReplication(Context* context) :
    Sample(context)
	, mainImageData_(context)
{
	SetRandomSeed(Time::GetSystemTime());
	color_.r_ = Random();
	color_.g_ = Random();
	color_.b_ = Random();

	mainImageData_.SetSize(MAIN_IMAGE_WIDTH, MAIN_IMAGE_HEIGHT,4);
}

void SceneReplication::Start()
{
    // Execute base class startup
    Sample::Start();

    // Create the scene content
    CreateScene();

    // Create the UI content
    CreateUI();

    // Hook up to necessary events
    SubscribeToEvents();

    // Set the mouse mode to use in the sample
    Sample::InitMouseMode(MM_RELATIVE);
}

void SceneReplication::CreateScene()
{
    scene_ = new Scene(context_);

	mainTexture_ = new Texture2D(context_);
	mainTexture_->SetNumLevels(1);
	mainTexture_->SetSize(MAIN_IMAGE_WIDTH, MAIN_IMAGE_HEIGHT, Graphics::GetRGBAFormat(), TEXTURE_DYNAMIC);
	mainTexture_->SetFilterMode(FILTER_NEAREST);
	mainTexture_->SetName("DrawTexture");
}

void SceneReplication::CreateUI()
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    UI* ui = GetSubsystem<UI>();
    UIElement* root = ui->GetRoot();
    XMLFile* uiStyle = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");
    // Set style to the UI root so that elements will inherit it
    root->SetDefaultStyle(uiStyle);

    // Create a Cursor UI element because we want to be able to hide and show it at will. When hidden, the mouse cursor will
    // control the camera, and when visible, it can interact with the login UI
    SharedPtr<Cursor> cursor(new Cursor(context_));
    cursor->SetStyleAuto(uiStyle);
    ui->SetCursor(cursor);
    // Set starting position of the cursor at the rendering window center
    Graphics* graphics = GetSubsystem<Graphics>();
    cursor->SetPosition(graphics->GetWidth() / 2, graphics->GetHeight() / 2);

    // Construct the instructions text element
    instructionsText_ = ui->GetRoot()->CreateChild<Text>();
    instructionsText_->SetText(
        "Use mouse to draw image"
    );
    instructionsText_->SetFont(cache->GetResource<Font>("Fonts/Anonymous Pro.ttf"), 15);
    // Position the text relative to the screen center
    instructionsText_->SetHorizontalAlignment(HA_CENTER);
    instructionsText_->SetVerticalAlignment(VA_CENTER);
    instructionsText_->SetPosition(0, graphics->GetHeight() / 4);
    // Hide until connected
    instructionsText_->SetVisible(false);

    buttonContainer_ = root->CreateChild<UIElement>();
    buttonContainer_->SetFixedSize(500, 20);
    buttonContainer_->SetPosition(20, 20);
    buttonContainer_->SetLayoutMode(LM_HORIZONTAL);

    textEdit_ = buttonContainer_->CreateChild<LineEdit>();
    textEdit_->SetStyleAuto();

    connectButton_ = CreateButton("Connect", 90);
    disconnectButton_ = CreateButton("Disconnect", 100);
    startServerButton_ = CreateButton("Start Server", 110);

	mainImage_ = new Button(context_);
	mainImage_->SetTexture(mainTexture_); // Set texture
	mainImage_->SetFullImageRect();
	mainImage_->SetBlendMode(BLEND_REPLACE);
	mainImage_->SetPosition(200, 200);
	mainImage_->SetSize(MAIN_IMAGE_WIDTH, MAIN_IMAGE_HEIGHT);
	mainImage_->SetName("MainImage");
	root->AddChild(mainImage_);

	UpdateButtons();
}

void SceneReplication::SubscribeToEvents()
{
    // Subscribe to button actions
    SubscribeToEvent(connectButton_, E_RELEASED, URHO3D_HANDLER(SceneReplication, HandleConnect));
    SubscribeToEvent(disconnectButton_, E_RELEASED, URHO3D_HANDLER(SceneReplication, HandleDisconnect));
    SubscribeToEvent(startServerButton_, E_RELEASED, URHO3D_HANDLER(SceneReplication, HandleStartServer));

    // Subscribe to network events
    SubscribeToEvent(E_SERVERCONNECTED, URHO3D_HANDLER(SceneReplication, HandleConnectionStatus));
    SubscribeToEvent(E_SERVERDISCONNECTED, URHO3D_HANDLER(SceneReplication, HandleConnectionStatus));
    SubscribeToEvent(E_CONNECTFAILED, URHO3D_HANDLER(SceneReplication, HandleConnectionStatus));
    SubscribeToEvent(E_CLIENTCONNECTED, URHO3D_HANDLER(SceneReplication, HandleClientConnected));
    SubscribeToEvent(E_CLIENTDISCONNECTED, URHO3D_HANDLER(SceneReplication, HandleClientDisconnected));

	SubscribeToEvent(mainImage_, E_CLICK, URHO3D_HANDLER(SceneReplication, HandleMouseDraw));
	
	// This is a custom event, sent from the server to the client. It tells the node ID of the object the client should control
	SubscribeToEvent(E_REFRESH_POINT, URHO3D_HANDLER(SceneReplication, HandleRefreshPoint));
	SubscribeToEvent(E_REFRESH_TEXTURE, URHO3D_HANDLER(SceneReplication, HandleRefreshTexture));

	// Events sent between client & server (remote events) must be explicitly registered or else they are not allowed to be received
	GetSubsystem<Network>()->RegisterRemoteEvent(E_REFRESH_POINT);
	GetSubsystem<Network>()->RegisterRemoteEvent(E_REFRESH_TEXTURE);
}

Button* SceneReplication::CreateButton(const String& text, int width)
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    Font* font = cache->GetResource<Font>("Fonts/Anonymous Pro.ttf");

    Button* button = buttonContainer_->CreateChild<Button>();
    button->SetStyleAuto();
    button->SetFixedWidth(width);

    Text* buttonText = button->CreateChild<Text>();
    buttonText->SetFont(font, 12);
    buttonText->SetAlignment(HA_CENTER, VA_CENTER);
    buttonText->SetText(text);

    return button;
}

void SceneReplication::UpdateButtons()
{
    Network* network = GetSubsystem<Network>();
    Connection* serverConnection = network->GetServerConnection();
    bool serverRunning = network->IsServerRunning();

    // Show and hide buttons so that eg. Connect and Disconnect are never shown at the same time
    connectButton_->SetVisible(!serverConnection && !serverRunning);
    disconnectButton_->SetVisible(serverConnection || serverRunning);
    startServerButton_->SetVisible(!serverConnection && !serverRunning);
    textEdit_->SetVisible(!serverConnection && !serverRunning);

	mainImage_->SetVisible(serverConnection || serverRunning);

	mainImageData_.Clear(Color::WHITE);
	RefreshMainImage();
}

void SceneReplication::HandleConnect(StringHash eventType, VariantMap& eventData)
{
    Network* network = GetSubsystem<Network>();
    String address = textEdit_->GetText().Trimmed();
    if (address.Empty())
        address = "localhost"; // Use localhost to connect if nothing else specified

    // Connect to server, specify scene to use as a client for replication
    network->Connect(address, SERVER_PORT, scene_);

    UpdateButtons();
}

void SceneReplication::HandleDisconnect(StringHash eventType, VariantMap& eventData)
{
    Network* network = GetSubsystem<Network>();
    Connection* serverConnection = network->GetServerConnection();
    // If we were connected to server, disconnect. Or if we were running a server, stop it. In both cases clear the
    // scene of all replicated content, but let the local nodes & components (the static world + camera) stay
    if (serverConnection)
    {
        serverConnection->Disconnect();
        scene_->Clear(true, false);
    }
    // Or if we were running a server, stop it
    else if (network->IsServerRunning())
    {
        network->StopServer();
        scene_->Clear(true, false);
    }

    UpdateButtons();
}

void SceneReplication::HandleStartServer(StringHash eventType, VariantMap& eventData)
{
    Network* network = GetSubsystem<Network>();
    network->StartServer(SERVER_PORT);
    UpdateButtons();
}

void SceneReplication::HandleConnectionStatus(StringHash eventType, VariantMap& eventData)
{
    UpdateButtons();
}

void SceneReplication::HandleClientConnected(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientConnected;

    // When a client connects, assign to scene to begin scene replication
    Connection* newConnection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    newConnection->SetScene(scene_);
	VariantMap vm;
	vm[RefreshTexture::P_DATA].SetBuffer(mainImageData_.GetData(), MAIN_IMAGE_AREA*4);
	newConnection->SendRemoteEvent(E_REFRESH_TEXTURE, true, vm);
}

void SceneReplication::HandleClientDisconnected(StringHash eventType, VariantMap& eventData)
{
    using namespace ClientConnected;

    // When a client disconnects, remove the controlled object
    Connection* connection = static_cast<Connection*>(eventData[P_CONNECTION].GetPtr());
    Node* object = serverObjects_[connection];
    if (object)
        object->Remove();

    serverObjects_.Erase(connection);
}

void SceneReplication::HandleMouseDraw(StringHash eventType, VariantMap& eventData)
{
	auto p = mainImage_->GetPosition();
	p.x_ = eventData[Click::P_X].GetInt() - p.x_;
	p.y_ = eventData[Click::P_Y].GetInt() - p.y_;

	if (GetSubsystem<Network>()->IsServerRunning())
		AddPoint(p, color_);

	VariantMap args;
	args[RefreshPoint::P_POS] = p;
	args[RefreshPoint::P_COLOR] = color_;
	SendMyMessage(E_REFRESH_POINT, args);
}

void SceneReplication::SendMyMessage(StringHash eventType, const VariantMap& eventData)
{
	Network* network = GetSubsystem<Network>();
	if (auto serverConnection = network->GetServerConnection())
	{
		serverConnection->SendRemoteEvent(eventType, 0, eventData);
	}
	else if (network->IsServerRunning())
	{
		for (auto c : GetSubsystem<Network>()->GetClientConnections())
			c->SendRemoteEvent(eventType, 0, eventData);
	}
}


void SceneReplication::AddPoint(const IntVector2& p, const Color& cl)
{
	if (p.x_<0 || p.x_ >= MAIN_IMAGE_WIDTH)
		return;
	if (p.y_<0 || p.y_ >= MAIN_IMAGE_HEIGHT)
		return;

	enum { RADIUS = 10, RADIUS_SQR = RADIUS*RADIUS };

	auto x0 = std::max<int>(p.x_ - RADIUS, 0);
	auto x1 = std::min<int>(p.x_ + RADIUS, MAIN_IMAGE_WIDTH);

	auto y0 = std::max<int>(p.y_ - RADIUS, 0);
	auto y1 = std::min<int>(p.y_ + RADIUS, MAIN_IMAGE_WIDTH);

	for (int i = x0; i < x1; ++i)
	{
		auto dx = p.x_ - i;
		auto dx_sqr = dx*dx;
		for (int j = y0; j < y1; ++j)
		{
			auto dy = p.y_ - j;
			auto dy_sqr = dy*dy;
			if (dx_sqr + dy_sqr > RADIUS_SQR)
				continue;
			mainImageData_.SetPixel(i, j, cl);
		}
	}
	RefreshMainImage();
}

void SceneReplication::HandleRefreshPoint(StringHash eventType, VariantMap& eventData)
{
	auto pos = eventData[RefreshPoint::P_POS].GetIntVector2();
	auto cl = eventData[RefreshPoint::P_COLOR].GetColor();
	AddPoint(pos, cl);
	if (GetSubsystem<Network>()->IsServerRunning())
		SendMyMessage(E_REFRESH_POINT, eventData);
}

void SceneReplication::HandleRefreshTexture(StringHash eventType, VariantMap& eventData)
{
	auto& buf = eventData[RefreshTexture::P_DATA].GetBuffer();
	if (buf.Size() != MAIN_IMAGE_AREA*4)
		return;
	mainImageData_.SetData(&buf.At(0));
	RefreshMainImage();
}


void SceneReplication::RefreshMainImage()
{
	mainTexture_->SetData(0,0,0, MAIN_IMAGE_WIDTH, MAIN_IMAGE_HEIGHT,mainImageData_.GetData());
}
