/* ShipyardPanel.cpp
Copyright (c) 2014 by Michael Zahniser

Endless Sky is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

Endless Sky is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.
*/

#include "ShipyardPanel.h"

#include "Dialog.h"
#include "FillShader.h"
#include "Font.h"
#include "FontSet.h"
#include "GameData.h"
#include "PlayerInfo.h"
#include "Point.h"
#include "Screen.h"
#include "Sprite.h"
#include "SpriteSet.h"
#include "SpriteShader.h"
#include "UI.h"

#include <algorithm>

using namespace std;

namespace {
	static const int SIDE_WIDTH = 250;
	static const int TILE_SIZE = 250;
	
	static const vector<string> CATEGORIES = {
		"Light Freighter",
		"Interceptor",
		"Heavy Freighter",
		"Light Warship",
		"Heavy Warship",
		"Fighter",
		"Drone"
	};
	
	// Draw the given ship at the given location, zoomed so it will fit within
	// one cell of the grid.
	void DrawShip(const Ship &ship, const Point &center, bool isSelected)
	{
		const Sprite *sprite = ship.GetSprite().GetSprite();
		const Sprite *back = SpriteSet::Get(
			isSelected ? "ui/shipyard selected" : "ui/shipyard unselected");
		SpriteShader::Draw(back, center);
		// Make sure the ship sprite leaves 10 pixels padding all around.
		float zoomSize = TILE_SIZE - 60.f;
		
		// Draw the ship name.
		const string &name = ship.Name().empty() ? ship.ModelName() : ship.Name();
		const Font &font = FontSet::Get(14);
		Point offset(-.5f * font.Width(name), -.5f * TILE_SIZE + 10.f);
		font.Draw(name, center + offset, Color(.8, 0.));
		
		float zoom = min(.5f, zoomSize / max(sprite->Width(), sprite->Height()));
		SpriteShader::Draw(sprite, center, zoom);
	}
}



ShipyardPanel::ShipyardPanel(const GameData &data, PlayerInfo &player)
	: data(data), player(player), planet(player.GetPlanet()),
	playerShip(player.GetShip()), selectedShip(nullptr),
	mainScroll(0), sideScroll(0)
{
	SetIsFullScreen(true);
	
	if(playerShip)
		playerShipInfo.Update(*playerShip);
	
	for(const pair<string, Ship> &it : data.Ships())
		catalog[it.second.Attributes().Category()].insert(it.first);
}



void ShipyardPanel::Draw() const
{
	glClear(GL_COLOR_BUFFER_BIT);
	
	const Font &font = FontSet::Get(14);
	Color bright(.8, 0.);
	Color dim(.5, 0.);
	
	// First, draw the side panel.
	FillShader::Fill(
		Point((Screen::Width() - SIDE_WIDTH) * .5, 0.),
		Point(SIDE_WIDTH, Screen::Height()),
		Color(.1, 1.));
	FillShader::Fill(
		Point(Screen::Width() * .5 - SIDE_WIDTH, 0.),
		Point(1, Screen::Height()),
		Color(.2, 1.));
	
	// Clear the list of clickable zones.
	zones.clear();
	
	static const string YOURS = "Your Ships:";
	Point yoursPoint(
		(Screen::Width() - SIDE_WIDTH - font.Width(YOURS)) / 2,
		Screen::Height() / -2 + 10 - sideScroll);
	font.Draw(YOURS, yoursPoint, bright);
	
	Point point(
		(Screen::Width() - SIDE_WIDTH) / 2,
		(Screen::Height() - SIDE_WIDTH) / -2 - sideScroll + 40);
	for(shared_ptr<Ship> ship : player.Ships())
	{
		// Skip any ships that are "absent" for whatever reason.
		if(ship->GetSystem() != player.GetSystem())
			continue;
		
		bool isSelected = (ship.get() == playerShip);
		DrawShip(*ship, point, isSelected);
		zones.emplace_back(point.X(), point.Y(), TILE_SIZE / 2, TILE_SIZE / 2, ship.get());
		
		if(isSelected)
		{
			Point offset(SIDE_WIDTH / -2, TILE_SIZE / 2);
			
			playerShipInfo.DrawSale(point + offset);
			point.Y() += playerShipInfo.SaleHeight();
		}
		point.Y() += TILE_SIZE;
	}
	maxSideScroll = point.Y() + sideScroll - (Screen::Height()) / 2 + 70 - TILE_SIZE / 2;
	maxSideScroll = max(0, maxSideScroll);
	
	// The last 70 pixels on the end of the side panel are for the buttons:
	FillShader::Fill(
		Point((Screen::Width() - SIDE_WIDTH) / 2, Screen::Height() / 2 - 35),
		Point(SIDE_WIDTH, 70), Color(.2, 1.));
	FillShader::Fill(
		Point((Screen::Width() - SIDE_WIDTH) / 2, Screen::Height() / 2 - 70),
		Point(SIDE_WIDTH, 1), Color(.3, 1.));
	point.Set(
		Screen::Width() / 2 - SIDE_WIDTH + 10,
		Screen::Height() / 2 - 65);
	font.Draw("You have:", point, dim);
	string credits = to_string(player.Accounts().Credits()) + " credits";
	point.X() += (SIDE_WIDTH - 20) - font.Width(credits);
	font.Draw(credits, point, bright);
	
	const Font &bigFont = FontSet::Get(18);
	
	Point buyCenter(Screen::Width() / 2 - 210, Screen::Height() / 2 - 25);
	FillShader::Fill(buyCenter, Point(60, 30), Color(.1, 1.));
	bool canBuy = selectedShip && (selectedShip->Cost() <= player.Accounts().Credits());
	bigFont.Draw("Buy",
		buyCenter - .5 * Point(bigFont.Width("Buy"), bigFont.Height()),
		canBuy ? bright : dim);
	
	Point sellCenter(Screen::Width() / 2 - 130, Screen::Height() / 2 - 25);
	FillShader::Fill(sellCenter, Point(60, 30), Color(.1, 1.));
	bigFont.Draw("Sell",
		sellCenter - .5 * Point(bigFont.Width("Sell"), bigFont.Height()),
		playerShip ? bright : dim);
	
	Point leaveCenter(Screen::Width() / 2 - 45, Screen::Height() / 2 - 25);
	FillShader::Fill(leaveCenter, Point(70, 30), Color(.1, 1.));
	bigFont.Draw("Leave",
		leaveCenter - .5 * Point(bigFont.Width("Leave"), bigFont.Height()),
		bright);
	
	// Draw all the available ships.
	// First, figure out how many colums we can draw.
	int mainWidth = (Screen::Width() - SIDE_WIDTH - 1);
	int columns = mainWidth / TILE_SIZE;
	int columnWidth = mainWidth / columns;
	
	Point begin(
		(Screen::Width() - columnWidth) / -2,
		(Screen::Height() - TILE_SIZE) / -2 - mainScroll);
	point = begin;
	float endX = Screen::Width() * .5f - (SIDE_WIDTH + 1);
	double nextY = begin.Y() + TILE_SIZE;
	for(const string &category : CATEGORIES)
	{
		map<string, set<string>>::const_iterator it = catalog.find(category);
		if(it == catalog.end())
			continue;
		
		// This should never happen, but bail out if we don't know what planet
		// we are on (meaning there's no way to know what ships are for sale).
		if(!planet)
			break;
		
		Point side(Screen::Width() * -.5 + 10., point.Y() - TILE_SIZE / 2 + 10);
		point.Y() += bigFont.Height() + 20;
		nextY += bigFont.Height() + 20;
		
		bool isEmpty = true;
		for(const string &name : it->second)
		{
			const Ship *ship = data.Ships().Get(name);
			if(!planet->Shipyard().Has(ship))
				continue;
			isEmpty = false;
			
			bool isSelected = (ship == selectedShip);
			DrawShip(*ship, point, isSelected);
			zones.emplace_back(point.X(), point.Y(), columnWidth / 2, TILE_SIZE / 2, ship);
		
			if(isSelected)
			{
				Color color(.2, 1.);
			
				float before = point.X() - TILE_SIZE / 2 - Screen::Width() * -.5;
				FillShader::Fill(Point(Screen::Width() * -.5 + .5 * before, point.Y() + 121.),
					Point(before, 1.), color);
			
				float after = endX - (point.X() + TILE_SIZE / 2);
				FillShader::Fill(Point(endX - .5 * after, point.Y() + 121.),
					Point(after, 1.), color);
			
				// The center of the display needs to be between these two values:
				int panelAndAHalf = (selectedShipInfo.PanelWidth() * 3) / 2;
				double minX = Screen::Width() / -2 + panelAndAHalf;
				double maxX = Screen::Width() / -2 + mainWidth - panelAndAHalf;
				Point center(
					max(minX, min(maxX, point.X())) - selectedShipInfo.PanelWidth() / 2,
					point.Y() + TILE_SIZE / 2);
				Point offset(selectedShipInfo.PanelWidth(), 0.);
			
				selectedShipInfo.DrawDescription(center - offset);
				selectedShipInfo.DrawAttributes(center);
				selectedShipInfo.DrawOutfits(center + offset);
			
				nextY += selectedShipInfo.MaximumHeight();
			}
		
			point.X() += columnWidth;
			if(point.X() >= endX)
			{
				point.X() = begin.X();
				point.Y() = nextY;
				nextY += TILE_SIZE;
			}
		}
		
		if(!isEmpty)
		{
			bigFont.Draw(category, side, bright);
			
			if(point.X() != begin.X())
			{
				point.X() = begin.X();
				point.Y() = nextY;
				nextY += TILE_SIZE;
			}
			point.Y() += 40;
			nextY += 40;
		}
		else
		{
			point.Y() -= bigFont.Height() + 20;
			nextY -= bigFont.Height() + 20;
		}
	}
	// This is how much Y space was actually used.
	nextY -= 40 + TILE_SIZE;
	
	// What amount would mainScroll have to equal to make nextY equal the
	// bottom of the screen?
	maxMainScroll = nextY + mainScroll - Screen::Height() / 2 - TILE_SIZE / 2;
	maxMainScroll = max(0, maxMainScroll);
}



// Only override the ones you need; the default action is to return false.
bool ShipyardPanel::KeyDown(SDLKey key, SDLMod mod)
{
	if(key == 'l')
		GetUI()->Pop(this);
	else if(key == 'b')
	{
		if(!selectedShip || player.Accounts().Credits() < selectedShip->Cost())
			return false;
		
		GetUI()->Push(new Dialog(*this, &ShipyardPanel::BuyShip,
			"Enter a name for your brand new " + selectedShip->ModelName() + "!"));
	}
	else if(key == 's')
	{
		if(!playerShip)
			return false;
		
		GetUI()->Push(new Dialog(*this, &ShipyardPanel::SellShip,
			"Sell ''" + playerShip->Name() + "''?"));
	}
	else
		return false;
	
	return true;
}



bool ShipyardPanel::Click(int x, int y)
{
	if(x >= Screen::Width() / 2 - SIDE_WIDTH && y >= Screen::Height() / 2 - 70)
	{
		x -= Screen::Width() / 2 - SIDE_WIDTH;
		if(x < 80)
			KeyDown(SDLK_b, KMOD_NONE);
		else if(x < 160)
			KeyDown(SDLK_s, KMOD_NONE);
		else
			KeyDown(SDLK_l, KMOD_NONE);
		
		return true;
	}
	
	dragMain = (x < Screen::Width() / 2 - SIDE_WIDTH);
	for(const ClickZone &zone : zones)
		if(zone.Contains(x, y))
		{
			if(dragMain)
			{
				selectedShip = zone.GetShip();
				selectedShipInfo.Update(*selectedShip);
			}
			else
			{
				playerShip = zone.GetShip();
				playerShipInfo.Update(*playerShip);
			}
		}
		
	return true;
}



bool ShipyardPanel::Drag(int dx, int dy)
{
	int &scroll = dragMain ? mainScroll : sideScroll;
	const int &maximum = dragMain ? maxMainScroll : maxSideScroll;
	
	scroll = max(0, min(maximum, scroll - dy));
	return true;
}



bool ShipyardPanel::Scroll(int x, int y, int dy)
{
	bool inMain = (x < Screen::Width() / 2 - SIDE_WIDTH);
	int &scroll = inMain ? mainScroll : sideScroll;
	const int &maximum = inMain ? maxMainScroll : maxSideScroll;
	
	scroll = max(0, min(maximum, scroll - 50 * dy));
	return true;
}



ShipyardPanel::ClickZone::ClickZone(int x, int y, int rx, int ry, const Ship *ship)
	: left(x - rx), top(y - ry), right(x + rx), bottom(y + ry), ship(ship)
{
}



bool ShipyardPanel::ClickZone::Contains(int x, int y) const
{
	return (x >= left && x < right && y >= top && y < bottom);
}



const Ship *ShipyardPanel::ClickZone::GetShip() const
{
	return ship;
}



void ShipyardPanel::BuyShip(const std::string &name)
{
	if(name.empty())
		player.BuyShip(selectedShip, "Unnamed Ship");
	else
		player.BuyShip(selectedShip, name);
}



void ShipyardPanel::SellShip()
{
	player.SellShip(playerShip);
	playerShip = nullptr;
}
