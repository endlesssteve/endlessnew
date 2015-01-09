/* Ship.cpp
Copyright (c) 2014 by Michael Zahniser

Endless Sky is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

Endless Sky is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.
*/

#include "Ship.h"

#include "DataNode.h"
#include "DataWriter.h"
#include "GameData.h"
#include "Government.h"
#include "Mask.h"
#include "Messages.h"
#include "Projectile.h"
#include "Random.h"
#include "ShipEvent.h"
#include "SpriteSet.h"
#include "System.h"

#include <algorithm>
#include <cassert>
#include <cmath>

using namespace std;



void Ship::Load(const DataNode &node)
{
	assert(node.Size() >= 2 && node.Token(0) == "ship");
	modelName = node.Token(1);
	
	government = GameData::PlayerGovernment();
	equipped.clear();
	
	// Note: I do not clear the attributes list here so that it is permissible
	// to override one ship definition with another.
	for(const DataNode &child : node)
	{
		if(child.Token(0) == "sprite")
			sprite.Load(child);
		else if(child.Token(0) == "name" && child.Size() >= 2)
			name = child.Token(1);
		else if(child.Token(0) == "attributes")
			baseAttributes.Load(child);
		else if(child.Token(0) == "engine" && child.Size() >= 3)
			enginePoints.emplace_back(child.Value(1), child.Value(2));
		else if((child.Token(0) == "gun" || child.Token(0) == "turret") && child.Size() >= 3)
		{
			Point hardpoint(child.Value(1), child.Value(2));
			const Outfit *outfit = nullptr;
			if(child.Size() >= 4)
			{
				outfit = GameData::Outfits().Get(child.Token(3));
				++equipped[outfit];
			}
			if(child.Token(0) == "gun")
				armament.AddGunPort(hardpoint, outfit);
			else
				armament.AddTurret(hardpoint, outfit);
		}
		else if(child.Token(0) == "licenses")
		{
			const Government *gov = nullptr;
			if(child.Size() >= 2)
				gov = GameData::Governments().Get(child.Token(1));
			
			vector<string> &vec = licenses[gov];
			for(const DataNode &grand : child)
				vec.push_back(grand.Token(0));
		}
		else if(child.Token(0) == "fighter" && child.Size() >= 3)
			fighterBays.emplace_back(child.Value(1), child.Value(2));
		else if(child.Token(0) == "drone" && child.Size() >= 3)
			droneBays.emplace_back(child.Value(1), child.Value(2));
		else if(child.Token(0) == "explode" && child.Size() >= 2)
		{
			int count = (child.Size() >= 3) ? child.Value(2) : 1;
			explosionEffects[GameData::Effects().Get(child.Token(1))] += count;
			explosionTotal += count;
		}
		else if(child.Token(0) == "outfits")
		{
			for(const DataNode &grand : child)
			{
				int count = (grand.Size() >= 2) ? grand.Value(1) : 1;
				this->outfits[GameData::Outfits().Get(grand.Token(0))] += count;
			}
		}
		else if(child.Token(0) == "cargo")
			cargo.Load(child);
		else if(child.Token(0) == "crew" && child.Size() >= 2)
			crew = static_cast<int>(child.Value(1));
		else if(child.Token(0) == "fuel" && child.Size() >= 2)
			fuel = child.Value(1);
		else if(child.Token(0) == "shields" && child.Size() >= 2)
			shields = child.Value(1);
		else if(child.Token(0) == "hull" && child.Size() >= 2)
			hull = child.Value(1);
		else if(child.Token(0) == "position" && child.Size() >= 3)
			position = Point(child.Value(1), child.Value(2));
		else if(child.Token(0) == "system" && child.Size() >= 2)
			currentSystem = GameData::Systems().Get(child.Token(1));
		else if(child.Token(0) == "planet" && child.Size() >= 2)
		{
			zoom = 0.;
			landingPlanet = GameData::Planets().Get(child.Token(1));
		}
		else if(child.Token(0) == "description" && child.Size() >= 2)
		{
			description += child.Token(1);
			description += '\n';
		}
	}
	// Different ships dissipate heat at different rates.
	heatDissipation = baseAttributes.Get("heat dissipation");
	if(!heatDissipation)
		heatDissipation = .999;
	else
		heatDissipation = 1. - .001 * heatDissipation;
	
	baseAttributes.Reset("gun ports", armament.GunCount());
	baseAttributes.Reset("turret mounts", armament.TurretCount());
	// All copies of this ship should save pointers to the "explosion" weapon
	// definition stored safely in the ship model, which will not be destroyed
	// until GameData is when the program quits.
	explosionWeapon = &GameData::Ships().Get(modelName)->BaseAttributes();
}



// When loading a ship, some of the outfits it lists may not have been
// loaded yet. So, wait until everything has been loaded, then call this.
void Ship::FinishLoading()
{
	// TODO: any way to do this through lazy evaluation, instead of having to
	// call this function explicitly?
	attributes = baseAttributes;
	for(const auto &it : outfits)
	{
		attributes.Add(*it.first, it.second);
		if(it.first->IsWeapon())
		{
			int count = it.second;
			auto eit = equipped.find(it.first);
			if(eit != equipped.end())
				count -= eit->second;
			
			if(count)
				armament.Add(it.first, count);
		}
	}
	cargo.SetSize(attributes.Get("cargo space"));
	equipped.clear();
	armament.FinishLoading();
	
	// Recharge, but don't recharge crew or fuel if not in the parent's system.
	// Do not recharge if this ship's starting state was saved.
	if(!hull)
	{
		shared_ptr<const Ship> parent = GetParent();
		Recharge(!parent || currentSystem == parent->currentSystem);
	}
}



// Save a full description of this ship, as currently configured.
void Ship::Save(DataWriter &out) const
{
	out.Write("ship", modelName);
	out.BeginChild();
		out.Write("name", name);
		sprite.Save(out);
		
		for(const auto &it : licenses)
		{
			if(it.first)
				out.Write("licenses", it.first->GetName());
			else
				out.Write("licenses");
			
			out.BeginChild();
			for(const auto &license : it.second)
				out.Write(license);
			out.EndChild();
		}
		
		out.Write("attributes");
		out.BeginChild();
			out.Write("category", baseAttributes.Category());
			for(const auto &it : baseAttributes.Attributes())
				if(it.second)
					out.Write(it.first, it.second);
		out.EndChild();
		
		out.Write("outfits");
		out.BeginChild();
			for(const auto &it : outfits)
				if(it.first && it.second)
				{
					if(it.second == 1)
						out.Write(it.first->Name());
					else
						out.Write(it.first->Name(), it.second);
				}
		out.EndChild();
		
		cargo.Save(out);
		out.Write("crew", crew);
		out.Write("fuel", fuel);
		out.Write("shields", shields);
		out.Write("hull", hull);
		out.Write("position", position.X(), position.Y());
		
		for(const Point &point : enginePoints)
			out.Write("engine", point.X(), point.Y());
		for(const Armament::Weapon &weapon : armament.Get())
		{
			const char *type = (weapon.IsTurret() ? "turret" : "gun");
			if(weapon.GetOutfit())
				out.Write(type, 2. * weapon.GetPoint().X(), 2. * weapon.GetPoint().Y(),
					weapon.GetOutfit()->Name());
			else
				out.Write(type, 2. * weapon.GetPoint().X(), 2. * weapon.GetPoint().Y());
		}
		for(const Bay &bay : fighterBays)
			out.Write("fighter", 2. * bay.point.X(), 2. * bay.point.Y());
		for(const Bay &bay : droneBays)
			out.Write("drone", 2. * bay.point.X(), 2. * bay.point.Y());
		for(const auto &it : explosionEffects)
			if(it.first && it.second)
				out.Write("explode", it.first->Name(), it.second);
		
		if(currentSystem)
			out.Write("system", currentSystem->Name());
		else
		{
			shared_ptr<const Ship> parent = GetParent();
			if(parent && parent->currentSystem)
				out.Write("system", parent->currentSystem->Name());
		}
		if(landingPlanet)
			out.Write("planet", landingPlanet->Name());
	out.EndChild();
}



const string &Ship::ModelName() const
{
	return modelName;
}



// Get this ship's description.
const string &Ship::Description() const
{
	return description;
}



// Get this ship's cost.
int Ship::Cost() const
{
	return attributes.Cost();
}



// Get the licenses needed to buy or operate this ship.
const vector<string> &Ship::Licenses(const Government *government) const
{
	// Find out if we have any licenses specifically for this government. If
	// not, check if there are any universally required licenses.
	auto it = licenses.find(government);
	if(it == licenses.end())
		it = licenses.find(nullptr);
	
	static const vector<string> empty;
	return (it == licenses.end()) ? empty : it->second;
}



void Ship::Place(Point position, Point velocity, Angle angle)
{
	this->position = position;
	this->velocity = velocity;
	this->angle = angle;
	// If landed, place the ship right above the planet.
	if(landingPlanet)
		landingPlanet = nullptr;
	else
		zoom = 1.;
	forget = 1;
	if(government)
		sprite.SetSwizzle(government->GetSwizzle());
}



// Set the name of this particular ship.
void Ship::SetName(const string &name)
{
	this->name = name;
}



// Set which system this ship is in.
void Ship::SetSystem(const System *system)
{
	currentSystem = system;
}



void Ship::SetPlanet(const Planet *planet)
{
	zoom = 0.;
	landingPlanet = planet;
	SetDestination(nullptr);
}



void Ship::SetGovernment(const Government *government)
{
	if(government)
		sprite.SetSwizzle(government->GetSwizzle());
	this->government = government;
}



void Ship::SetIsSpecial(bool special)
{
	isSpecial = special;
}



const Personality &Ship::GetPersonality() const
{
	return personality;
}



void Ship::SetPersonality(const Personality &other)
{
	personality = other;
	if(personality.IsDerelict())
	{
		shields = 0.;
		hull = .5 * MinimumHull();
		isDisabled = true;
	}
}



string Ship::GetHail() const
{
	bool isEnemy = government->IsEnemy();
	const Phrase *name = hail[isEnemy];
	
	if(!name)
		return "";
	
	return name->Get();
}



void Ship::SetHail(const Phrase *friendly, const Phrase *hostile)
{
	hail[0] = friendly;
	hail[1] = hostile;
}



// Set the commands for this ship to follow this timestep.
void Ship::SetCommands(const Command &command)
{
	commands = command;
}



const Command &Ship::Commands() const
{
	return commands;
}



// Move this ship. A ship may create effects as it moves, in particular if
// it is in the process of blowing up. If this returns false, the ship
// should be deleted.
bool Ship::Move(list<Effect> &effects)
{
	// Check if this ship has been in a different system from the player for so
	// long that it should be "forgotten."
	forget += !isInSystem;
	if((!isSpecial && forget >= 1000) || !currentSystem)
		return false;
	isInSystem = false;
	if(!fuel || !(attributes.Get("hyperdrive") || attributes.Get("jump drive")))
		hyperspaceSystem = nullptr;
	
	
	// When ships recharge, what actually happens is that they can exceed their
	// maximum capacity for the rest of the turn, but must be clamped to the
	// maximum here before they gain more. This is so that, for example, a ship
	// with no batteries but a good generator can still move.
	energy = min(energy, attributes.Get("energy capacity"));
	
	heat *= heatDissipation;
	if(heat > Mass() * 100.)
		isOverheated = true;
	else if(heat < Mass() * 90.)
		isOverheated = false;
	
	double maxShields = attributes.Get("shields");
	shields = min(shields, attributes.Get("shields"));
	hull = min(hull, attributes.Get("hull"));
	isDisabled = isOverheated || IsDisabled();
	
	// Update ship supply levels.
	if(!isOverheated)
	{
		// Note: If the ship is disabled because of low hull percent, _and_ it
		// has the capability of repairing its hull, it can repair enough to
		// cease to be disabled.
		hull += attributes.Get("hull repair rate");
	}
	if(!isDisabled)
	{
		// If you have a ramscoop, you recharge enough fuel to make one jump in
		// a little less than a minute - enough to be an inconvenience without
		// being totally aggravating.
		if(attributes.Get("ramscoop"))
			TransferFuel(-.03 * sqrt(attributes.Get("ramscoop")), nullptr);
		
		energy += attributes.Get("energy generation");
		heat += attributes.Get("heat generation");
		heat -= attributes.Get("cooling");
		heat = max(0., heat);
		
		// Recharge shields, but only up to the max. If there is extra shield
		// energy, use it to recharge fighters and drones.
		shields += attributes.Get("shield generation");
		static const double SHIELD_EXCHANGE_RATE = 1.;
		energy -= SHIELD_EXCHANGE_RATE * attributes.Get("shield generation");
		double excessShields = max(0., shields - maxShields);
		shields -= excessShields;
		
		for(Bay &bay : fighterBays)
		{
			if(!bay.ship)
				continue;
			
			double myGen = bay.ship->Attributes().Get("shield generation");
			double myMax = bay.ship->Attributes().Get("shields");
			bay.ship->shields = min(myMax, bay.ship->shields + myGen);
			if(excessShields > 0. && bay.ship->shields < myMax)
			{
				double extra = min(myMax - bay.ship->shields, excessShields);
				bay.ship->shields += extra;
				excessShields -= extra;
			}
		}
		for(Bay &bay : droneBays)
		{
			if(!bay.ship)
				continue;
			
			double myGen = bay.ship->Attributes().Get("shield generation");
			double myMax = bay.ship->Attributes().Get("shields");
			bay.ship->shields = min(myMax, bay.ship->shields + myGen);
			if(excessShields > 0. && bay.ship->shields < myMax)
			{
				double extra = min(myMax - bay.ship->shields, excessShields);
				bay.ship->shields += extra;
				excessShields -= extra;
			}
		}
		// If you do not need the shield generation, apply the extra back to
		// your energy. On the other hand, if recharging shields drives your
		// energy negative, undo that part of the recharge.
		energy += SHIELD_EXCHANGE_RATE * excessShields;
		if(energy < 0.)
		{
			shields += energy / SHIELD_EXCHANGE_RATE;
			energy = 0.;
		}
	}
	
	
	if(IsDestroyed())
	{
		// Once we've created enough little explosions, die.
		if(explosionCount == explosionTotal || forget)
		{
			if(!forget)
				for(unsigned i = 0; i < explosionTotal; ++i)
					CreateExplosion(effects);
			energy = 0.;
			heat = 0.;
			fuel = 0.;
			return false;
		}
		
		// If the ship is dead, it first creates explosions at an increasing
		// rate, then disappears in one big explosion.
		++explosionRate;
		if(Random::Int(1024) < explosionRate)
			CreateExplosion(effects);
	}
	else if(hyperspaceSystem || hyperspaceCount)
	{
		fuel -= (hyperspaceSystem != nullptr);
		
		// Enter hyperspace.
		int direction = (hyperspaceSystem != nullptr) - (hyperspaceSystem == nullptr);
		hyperspaceCount += direction;
		static const int HYPER_C = 100;
		static const double HYPER_A = 2.;
		bool hasJumpDrive = attributes.Get("jump drive");
		
		// Create the particle effects for the jump drive. This may create 100
		// or more particles per ship per turn at the peak of the jump.
		if(hasJumpDrive && !forget)
		{
			int count = hyperspaceCount;
			count *= sprite.Width() * sprite.Height();
			count /= 20000;
			const Effect *effect = GameData::Effects().Get("skylance impact");
			while(--count >= 0)
			{
				Point point((Random::Real() - .5) * .5 * sprite.Width(),
					(Random::Real() - .5) * .5 * sprite.Height());
				if(sprite.GetMask(0).Contains(point, Angle()))
				{
					effects.push_back(*effect);
					Point vel = velocity + 5. * Angle::Random(360.).Unit();
					effects.back().Place(angle.Rotate(point) + position, vel, angle);
				}
			}
		}
		
		if(hyperspaceCount == HYPER_C)
		{
			currentSystem = hyperspaceSystem;
			// If "jump fuel" is higher than 100, expend extra fuel now.
			fuel -= attributes.Get("jump fuel") - HYPER_C;
			hyperspaceSystem = nullptr;
			SetTargetSystem(nullptr);
			SetTargetPlanet(nullptr);
			direction = -1;
			
			if(hasJumpDrive)
				return true;
			
			Point target;
			for(const StellarObject &object : currentSystem->Objects())
				if(object.GetPlanet() && object.GetPlanet()->HasSpaceport())
				{
					target = object.Position();
					break;
				}
			if(GetDestination())
				for(const StellarObject &object : currentSystem->Objects())
					if(object.GetPlanet() == GetDestination())
					{
						target = object.Position();
						break;
					}
			
			// Have all ships exit hyperspace at the same distance so that
			// your escorts always stay with you.
			double distance = (HYPER_C * HYPER_C) * .5 * HYPER_A + 1000.;
			position = (target - distance * angle.Unit());
			position += hyperspaceOffset;
			// Make sure your velocity is in exactly the direction you are
			// traveling in, so that when you decelerate there will not be a
			// sudden shift in direction at the end.
			velocity = velocity.Length() * angle.Unit();
		}
		if(!hasJumpDrive)
		{
			velocity += (HYPER_A * direction) * angle.Unit();
			if(velocity.Length() <= MaxVelocity() && !hyperspaceSystem)
			{
				velocity = angle.Unit() * MaxVelocity();
				hyperspaceCount = 0;
			}
		}
		position += velocity;
		if(GetParent() && GetParent()->currentSystem == currentSystem)
		{
			hyperspaceOffset = position - GetParent()->position;
			double length = hyperspaceOffset.Length();
			if(length > 1000.)
				hyperspaceOffset *= 1000. / length;
		}
		
		return true;
	}
	else if(landingPlanet || zoom < 1.)
	{
		// Special ships do not disappear forever when they land; they
		// just slowly refuel.
		if(landingPlanet && zoom)
		{
			// Move the ship toward the center of the planet while landing.
			if(GetTargetPlanet())
				position = .97 * position + .03 * GetTargetPlanet()->Position();
			zoom -= .02;
			if(zoom < 0.)
			{
				// If this is not a special ship, it ceases to exist when it
				// lands on a true planet. If this is a wormhole, the ship is
				// instantly transported.
				if(landingPlanet->IsWormhole())
				{
					currentSystem = landingPlanet->WormholeDestination(currentSystem);
					for(const StellarObject &object : currentSystem->Objects())
						if(object.GetPlanet() == landingPlanet)
							position = object.Position();
					SetTargetPlanet(nullptr);
					landingPlanet = nullptr;
				}
				else if(!isSpecial || personality.IsFleeing())
					return false;
				
				zoom = 0.;
			}
		}
		// Only refuel if this planet has a spaceport.
		else if(fuel == attributes.Get("fuel capacity")
				|| !landingPlanet || !landingPlanet->HasSpaceport())
		{
			zoom = min(1., zoom + .02);
			landingPlanet = nullptr;
		}
		else
			fuel = min(fuel + 1., attributes.Get("fuel capacity"));
		
		// Move the ship at the velocity it had when it began landing, but
		// scaled based on how small it is now.
		position += velocity * zoom;
		
		return true;
	}
	if(commands.Has(Command::LAND) && CanLand())
		landingPlanet = GetTargetPlanet()->GetPlanet();
	else if(commands.Has(Command::JUMP) && CanHyperspace())
		hyperspaceSystem = GetTargetSystem();
	
	double cloakingSpeed = attributes.Get("cloak");
	bool canCloak = (zoom == 1. && !isDisabled && !hyperspaceCount && cloakingSpeed
		&& fuel >= attributes.Get("cloaking fuel")
		&& energy >= attributes.Get("cloaking energy"));
	if(commands.Has(Command::CLOAK) && canCloak)
	{
		cloak = min(1., cloak + cloakingSpeed);
		fuel -= attributes.Get("cloaking fuel");
		energy -= attributes.Get("cloaking energy");
	}
	else if(cloakingSpeed)
		cloak = max(0., cloak - cloakingSpeed);
	else
		cloak = 0.;
	
	int requiredCrew = RequiredCrew();
	if(pilotError)
		--pilotError;
	else if(pilotOkay)
		--pilotOkay;
	else if(requiredCrew && static_cast<int>(Random::Int(requiredCrew)) >= Crew())
	{
		pilotError = 30;
		Messages::Add("Your ship is moving erratically because you do not have enough crew to pilot it.");
	}
	else
		pilotOkay = 30;
	
	// This ship is not landing or entering hyperspace. So, move it. If it is
	// disabled, all it can do is slow down to a stop.
	double mass = Mass();
	if(isDisabled)
		velocity *= 1. - attributes.Get("drag") / mass;
	else if(!pilotError)
	{
		double thrustCommand = commands.Has(Command::FORWARD) - commands.Has(Command::BACK);
		if(thrustCommand)
		{
			// Check if we are able to apply this thrust.
			double cost = attributes.Get((thrustCommand > 0.) ?
				"thrusting energy" : "reverse thrusting energy");
			if(energy < cost)
				thrustCommand = 0.;
			else
			{
				// If a reverse thrust is commanded and the capability does not
				// exist, ignore it (do not even slow under drag).
				double thrust = attributes.Get((thrustCommand > 0.) ?
					"thrust" : "reverse thrust");
				if(!thrust)
					thrustCommand = 0.;
				else
				{
					energy -= cost;
					heat += attributes.Get((thrustCommand > 0.) ?
						"thrusting heat" : "reverse thrusting heat");
					velocity += angle.Unit() * (thrustCommand * thrust / mass);
				}
			}
		}
		bool applyAfterburner = commands.Has(Command::AFTERBURNER) && !CannotAct();
		if(applyAfterburner)
		{
			double thrust = attributes.Get("afterburner thrust");
			double cost = attributes.Get("afterburner fuel");
			double energyCost = attributes.Get("afterburner energy");
			if(!thrust || fuel < cost || energy < energyCost)
				applyAfterburner = false;
			else
			{
				heat += attributes.Get("afterburner heat");
				fuel -= cost;
				energy -= energyCost;
				velocity += angle.Unit() * thrust / mass;
				
				if(!forget)
				{
					const Effect *effect = GameData::Effects().Get("afterburner");
					for(const auto &it : outfits)
						if(it.first->Get("afterburner thrust") && !it.first->DieEffects().empty())
						{
							effect = it.first->DieEffects().begin()->first;
							break;
						}
					for(const Point &point : enginePoints)
					{
						Point pos = angle.Rotate(point) * .5 * Zoom() + position;
						effects.push_back(*effect);
						effects.back().Place(pos + velocity, velocity - 6. * angle.Unit(), angle);
					}
				}
			}
		}
		if(thrustCommand || applyAfterburner)
			velocity *= 1. - attributes.Get("drag") / mass;
		if(commands.Turn())
		{
			// Check if we are able to turn.
			double cost = attributes.Get("turning energy");
			if(energy < cost)
				commands.SetTurn(0.);
			else
			{
				energy -= cost;
				heat += attributes.Get("turning heat");
				angle += commands.Turn() * TurnRate();
			}
		}
	}
	
	// Boarding:
	if(isBoarding && (commands.Has(Command::FORWARD | Command::BACK) || commands.Turn()))
		isBoarding = false;
	shared_ptr<const Ship> target = (IsFighter() ? GetParent() : GetTargetShip());
	if(target && !isDisabled)
	{
		Point dp = (target->position - position);
		double distance = dp.Length();
		Point dv = (target->velocity - velocity);
		double speed = dv.Length();
		isBoarding |= (distance < 50. && speed < 1. && commands.Has(Command::BOARD));
		if(isBoarding && !IsFighter())
		{
			if(!target->IsDisabled() && government->IsEnemy(target->government))
				isBoarding = false;
			else if(target->IsDestroyed())
				isBoarding = false;
		}
		if(isBoarding && !pilotError)
		{
			Angle facing = angle;
			bool left = target->Unit().Cross(facing.Unit()) < 0.;
			double turn = left - !left;
			
			// Check if the ship will still be pointing to the same side of the target
			// angle if it turns by this amount.
			facing += TurnRate() * turn;
			bool stillLeft = target->Unit().Cross(facing.Unit()) < 0.;
			if(left != stillLeft)
				turn = 0.;
			angle += TurnRate() * turn;
			
			velocity += dv.Unit() * .1;
			position += dp.Unit() * .5;
			
			if(distance < 10. && speed < 1. && (IsFighter() || !turn))
			{
				isBoarding = false;
				hasBoarded = true;
			}
		}
	}
	
	// And finally: move the ship!
	position += velocity;
	
	return true;
}



// Launch any ships that are ready to launch.
void Ship::Launch(list<shared_ptr<Ship>> &ships)
{
	if(!commands.Has(Command::DEPLOY) || CannotAct())
		return;
	
	for(Bay &bay : fighterBays)
		if(bay.ship && !Random::Int(60))
		{
			ships.push_back(bay.ship);
			double maxV = bay.ship->MaxVelocity();
			Point v = velocity + (.3 * maxV) * angle.Unit() + (.2 * maxV) * Angle::Random().Unit();
			bay.ship->Place(position + angle.Rotate(bay.point), v, angle);
			bay.ship->SetSystem(currentSystem);
			bay.ship->SetParent(shared_from_this());
			AddEscort(bay.ship);
			bay.ship.reset();
		}
	for(Bay &bay : droneBays)
		if(bay.ship && !Random::Int(40))
		{
			ships.push_back(bay.ship);
			double maxV = bay.ship->MaxVelocity();
			Point v = velocity + (.3 * maxV) * angle.Unit() + (.2 * maxV) * Angle::Random().Unit();
			bay.ship->Place(position + angle.Rotate(bay.point), v, angle);
			bay.ship->SetSystem(currentSystem);
			bay.ship->SetParent(shared_from_this());
			AddEscort(bay.ship);
			bay.ship.reset();
		}
}



// Check if this ship is boarding another ship.
shared_ptr<Ship> Ship::Board(bool autoPlunder)
{
	if(!hasBoarded || CannotAct())
		return shared_ptr<Ship>();
	hasBoarded = false;
	
	shared_ptr<Ship> victim = GetTargetShip();
	if(!victim || victim->IsDestroyed())
		return shared_ptr<Ship>();
	
	// For a fighter, "board" means "return to ship."
	if(IsFighter())
	{
		if(victim->AddFighter(shared_from_this()))
		{
			currentSystem = nullptr;
			victim->RemoveEscort(this);
		}
		return shared_ptr<Ship>();
	}
	
	// Board a ship of your own government to repair/refuel it.
	if(!government->IsEnemy(victim->GetGovernment()))
	{
		SetShipToAssist(shared_ptr<Ship>());
		victim->hull = max(victim->hull, victim->MinimumHull());
		victim->isDisabled = false;
		// Transfer some fuel if needed.
		if(!victim->JumpsRemaining() && CanRefuel(*victim))
			TransferFuel(victim->attributes.Get("jump fuel"), victim.get());
		return autoPlunder ? shared_ptr<Ship>() : victim;
	}
	if(!victim->IsDisabled())
		return shared_ptr<Ship>();
	
	// If the boarding ship is the player, they will choose what to plunder.
	// Always take fuel if you can.
	victim->TransferFuel(victim->fuel, this);
	if(autoPlunder)
	{
		// Take any outfits that fit.
		for(auto &it : victim->outfits)
			while(it.second && cargo.Transfer(it.first, -1))
				--it.second;
		// Take any commodities that fit.
		victim->cargo.TransferAll(&cargo);
		// Stop targeting this ship.
		SetTargetShip(shared_ptr<Ship>());
		
		// Pause for two seconds before moving on.
		pilotError = 120;
	}
	
	return victim;
}



// Scan the target, if able and commanded to. Return a ShipEvent bitmask
// giving the types of scan that succeeded.
int Ship::Scan() const
{
	if(!commands.Has(Command::SCAN) || CannotAct())
		return 0;
	
	shared_ptr<const Ship> target = GetTargetShip();
	if(!target)
		return 0;
	
	int result = 0;
	double distance = (target->position - position).Length();
	if(distance < attributes.Get("cargo scan"))
		result |= ShipEvent::SCAN_CARGO;
	if(distance < attributes.Get("outfit scan"))
		result |= ShipEvent::SCAN_OUTFITS;
	
	return result;
}



// Fire any weapons that are ready to fire. If an anti-missile is ready,
// instead of firing here this function returns true and it can be fired if
// collision detection finds a missile in range.
bool Ship::Fire(list<Projectile> &projectiles)
{
	isInSystem = true;
	forget = 0;
	
	// A ship that is about to die creates a special single-turn "projectile"
	// representing its death explosion.
	if(explosionCount == explosionTotal && explosionWeapon)
		projectiles.emplace_back(position, explosionWeapon);
	
	if(CannotAct())
		return false;
	
	bool hasAntiMissile = false;
	
	const vector<Armament::Weapon> &weapons = armament.Get();
	for(unsigned i = 0; i < weapons.size(); ++i)
	{
		const Outfit *outfit = weapons[i].GetOutfit();
		if(outfit && CanFire(outfit))
		{
			if(outfit->WeaponGet("anti-missile"))
				hasAntiMissile = true;
			else if(commands.HasFire(i))
				armament.Fire(i, *this, projectiles);
		}
	}
	
	armament.Step(*this);
	
	return hasAntiMissile;
}



// Fire an anti-missile.
bool Ship::FireAntiMissile(const Projectile &projectile, list<Effect> &effects)
{
	if(CannotAct())
		return false;
	
	const vector<Armament::Weapon> &weapons = armament.Get();
	for(unsigned i = 0; i < weapons.size(); ++i)
	{
		const Outfit *outfit = weapons[i].GetOutfit();
		if(outfit && CanFire(outfit))
			if(armament.FireAntiMissile(i, *this, projectile, effects))
				return true;
	}
	
	return false;
}



const System *Ship::GetSystem() const
{
	return currentSystem;
}



// If the ship is landed, get the planet it has landed on.
const Planet *Ship::GetPlanet() const
{
	return zoom ? nullptr : landingPlanet;
}



bool Ship::IsTargetable() const
{
	return (zoom == 1. && !explosionRate && !forget && cloak < 1.);
}



bool Ship::IsOverheated() const
{
	return isOverheated;
}



bool Ship::IsDisabled() const
{
	double minimumHull = MinimumHull();
	bool needsCrew = RequiredCrew() != 0;
	return (hull < minimumHull || (!crew && needsCrew));
}



bool Ship::IsLanding() const
{
	return landingPlanet;
}



bool Ship::IsHyperspacing() const
{
	return hyperspaceSystem;
}



// Check if this ship is currently able to begin landing on its target.
bool Ship::CanLand() const
{
	if(!GetTargetPlanet() || isDisabled || IsDestroyed())
		return false;
	
	if(!GetTargetPlanet()->GetPlanet()->CanLand(*this))
		return false;
	
	Point distance = GetTargetPlanet()->Position() - position;
	double speed = velocity.Length();
	
	return (speed < 1. && distance.Length() < GetTargetPlanet()->Radius());
}



// Check if this ship is currently able to enter hyperspace to it target.
bool Ship::CanHyperspace() const
{
	if(IsDisabled())
		return false;
	if(!GetTargetSystem())
		return false;
	if(fuel < attributes.Get("jump fuel"))
		return false;
	
	Point direction = GetTargetSystem()->Position() - currentSystem->Position();
	
	// The ship can only enter hyperspace if it is traveling slowly enough
	// and pointed in the right direction.
	if(attributes.Get("scram drive"))
	{
		double deviation = fabs(direction.Unit().Cross(velocity));
		if(deviation > attributes.Get("scram drive"))
			return false;
	}
	else if(velocity.Length() > attributes.Get("jump speed"))
		return false;
	
	if(attributes.Get("jump drive"))
		return true;
	if(!attributes.Get("hyperdrive"))
		return false;
	
	// Figure out if we're within one turn step of facing this system.
	bool left = direction.Cross(angle.Unit()) < 0.;
	Angle turned = angle + TurnRate() * (left - !left);
	bool stillLeft = direction.Cross(turned.Unit()) < 0.;
	
	return (left != stillLeft);
}



bool Ship::IsBoarding() const
{
	return isBoarding;
}



double Ship::Cloaking() const
{
	return cloak;
}



const Animation &Ship::GetSprite() const
{
	return sprite;
}



// Get the ship's government.
const Government *Ship::GetGovernment() const
{
	return government;
}



double Ship::Zoom() const
{
	return zoom;
}



const string &Ship::Name() const
{
	return name;
}



// Get the points from which engine flares should be drawn. If the ship is
// not thrusting right now, this will be empty.
const vector<Point> &Ship::EnginePoints() const
{
	static const vector<Point> empty;
	if(!commands.Has(Command::FORWARD) || isDisabled || attributes.FlareSprite().IsEmpty())
		return empty;
	
	return enginePoints;
}



// Get the sprite to be used for an engine flare, if the engines are firing
// at the moment. Otherwise, this returns null.
const Animation &Ship::FlareSprite() const
{
	return attributes.FlareSprite();
}



const Point &Ship::Position() const
{
	return position;
}



const Point &Ship::Velocity() const
{
	return velocity;
}



const Angle &Ship::Facing() const
{
	return angle;
}



// Get the facing unit vector times the scale factor.
Point Ship::Unit() const
{
	return angle.Unit() * (zoom * .5);
}



// Recharge and repair this ship (e.g. because it has landed).
void Ship::Recharge(bool atSpaceport)
{
	if(IsDestroyed())
		return;
	
	if(atSpaceport)
	{
		crew = max(crew, RequiredCrew());
		fuel = attributes.Get("fuel capacity");
	}
	pilotError = 0;
	pilotOkay = 0;
	
	if(!personality.IsDerelict())
	{
		shields = attributes.Get("shields");
		hull = attributes.Get("hull");
		energy = attributes.Get("energy capacity");
	}
	heat = max(0., attributes.Get("heat generation") - attributes.Get("cooling")) / (1. - heatDissipation);
}



// Mark a ship as destroyed.
void Ship::Destroy()
{
	hull = -1.;
}



// Check if this ship has been destroyed.
bool Ship::IsDestroyed() const
{
	return (hull < 0.);
}



// Get characteristics of this ship, as a fraction between 0 and 1.
double Ship::Shields() const
{
	double maximum = attributes.Get("shields");
	return maximum ? min(1., shields / maximum) : 0.;
}



double Ship::Hull() const
{
	double maximum = attributes.Get("hull");
	return maximum ? min(1., hull / maximum) : 1.;
}



double Ship::Fuel() const
{
	double maximum = attributes.Get("fuel capacity");
	return maximum ? min(1., fuel / maximum) : 0.;
}



int Ship::JumpsRemaining() const
{
	double jumpFuel = attributes.Get("jump fuel");
	if(!jumpFuel)
		return 0;
	return fuel / jumpFuel;
}



double Ship::Energy() const
{
	double maximum = attributes.Get("energy capacity");
	return maximum ? min(1., energy / maximum) : (hull > 0.) ? 1. : 0.;
}



double Ship::Heat() const
{
	double maximum = Mass() * 100.;
	return maximum ? min(1., heat / maximum) : 1.;
}



int Ship::Crew() const
{
	return crew;
}



int Ship::RequiredCrew() const
{
	// Drones do not need crew, but all other ships need at least one.
	return max(attributes.Category() == "Drone" ? 0 : 1,
		static_cast<int>(attributes.Get("required crew")));
}



void Ship::AddCrew(int count)
{
	crew += count;
}



bool Ship::CanRefuel(const Ship &other) const
{
	double needed = other.attributes.Get("jump fuel");
	return (fuel - needed >= attributes.Get("jump fuel"));
}



double Ship::TransferFuel(double amount, Ship *to)
{
	amount = max(fuel - attributes.Get("fuel capacity"), amount);
	if(to)
	{
		amount = min(to->attributes.Get("fuel capacity") - to->fuel, amount);
		to->fuel += amount;
	}
	fuel -= amount;
	return amount;
}



void Ship::WasCaptured(const shared_ptr<Ship> &capturer)
{
	// Repair up to the point where it is just barely not disabled.
	hull = max(hull, MinimumHull());
	
	// Set the new government.
	government = capturer->GetGovernment();
	
	// Transfer some crew over. Only transfer the bare minimum unless even that
	// is not possible, in which case, share evenly.
	int totalRequired = capturer->RequiredCrew() + RequiredCrew();
	int transfer = RequiredCrew();
	if(totalRequired > capturer->Crew())
		transfer = max(1, (capturer->Crew() * RequiredCrew()) / totalRequired);
	capturer->AddCrew(-transfer);
	AddCrew(transfer);
	
	// Set the capturer as this ship's parent.
	SetParent(capturer);
	SetTargetShip(weak_ptr<Ship>());
	SetTargetPlanet(nullptr);
	SetTargetSystem(nullptr);
	commands.Clear();
	isDisabled = false;
	hyperspaceSystem = nullptr;
	// TODO: add as an "escort" to this ship.
	
	isSpecial = capturer->isSpecial;
	personality = capturer->personality;
}



// Check if this ship should be deleted.
bool Ship::ShouldDelete() const
{
	return (!zoom && !isSpecial) || (IsDestroyed() && explosionCount >= explosionTotal);
}



double Ship::Mass() const
{
	double carried = 0.;
	for(const Bay &bay : droneBays)
		if(bay.ship)
			carried += bay.ship->Mass();
	for(const Bay &bay : fighterBays)
		if(bay.ship)
			carried += bay.ship->Mass();
	return carried + cargo.Used() + attributes.Get("mass");
}



double Ship::TurnRate() const
{
	return attributes.Get("turn") / Mass();
}



double Ship::Acceleration() const
{
	return attributes.Get("thrust") / Mass();
}



double Ship::MaxVelocity() const
{
	// v * drag / mass == thrust / mass
	// v * drag == thrust
	// v = thrust / drag
	return attributes.Get("thrust") / attributes.Get("drag");
}



// This ship just got hit by the given projectile. Take damage according to
// what sort of weapon the projectile it.
int Ship::TakeDamage(const Projectile &projectile, bool isBlast)
{
	int type = 0;
	
	const Outfit &weapon = projectile.GetWeapon();
	double shieldDamage = weapon.WeaponGet("shield damage");
	double hullDamage = weapon.WeaponGet("hull damage");
	double hitForce =  weapon.WeaponGet("hit force");
	double heatDamage = weapon.WeaponGet("heat damage");
	bool wasDisabled = IsDisabled();
	bool wasDestroyed = IsDestroyed();
	
	isBoarding = false;
	
	if(shields > shieldDamage)
	{
		shields -= shieldDamage;
		heat += .5 * heatDamage;
	}
	else if(!shields || shieldDamage)
	{
		if(shieldDamage)
		{
			hullDamage *= (1. - (shields / shieldDamage));
			shields = 0.;
		}
		hull -= hullDamage;
		heat += heatDamage;
	}
	
	if(hitForce)
	{
		Point d = position - projectile.Position();
		double distance = d.Length();
		if(distance)
			ApplyForce((hitForce / distance) * d);
	}
	
	if(!wasDisabled && IsDisabled())
		type |= ShipEvent::DISABLE;
	if(!wasDestroyed && IsDestroyed())
		type |= ShipEvent::DESTROY;
	// If this ship was hit directly and did not consider itself an enemy of the
	// ship that hit it, it is now "provoked" against that government.
	if(!isBlast && projectile.GetGovernment() && !projectile.GetGovernment()->IsEnemy(government)
			&& (Shields() < .9 || Hull() < .9 || !personality.IsForbearing()))
		type |= ShipEvent::PROVOKE;
	
	return type;
}



// Apply a force to this ship, accelerating it. This might be from a weapon
// impact, or from firing a weapon, for example.
void Ship::ApplyForce(const Point &force)
{
	double currentMass = Mass();
	if(!currentMass)
		return;
	
	velocity += force / currentMass;
	double maxVelocity = MaxVelocity();
	double currentVelocity = velocity.Length();
	if(currentVelocity > maxVelocity)
		velocity *= maxVelocity / currentVelocity;
}



int Ship::FighterBaysFree() const
{
	int count = 0;
	for(const Bay &bay : fighterBays)
		count += !bay.ship;
	return count;
}



int Ship::DroneBaysFree() const
{
	int count = 0;
	for(const Bay &bay : droneBays)
		count += !bay.ship;
	return count;
}



bool Ship::AddFighter(const shared_ptr<Ship> &ship)
{
	if(!ship)
		return false;
	
	bool isFighter = ship->attributes.Category() == "Fighter";
	bool isDrone = ship->attributes.Category() == "Drone";
	if(!(isFighter || isDrone))
		return false;
	
	vector<Bay> &bays = isFighter ? fighterBays : droneBays;
	for(Bay &bay : bays)
		if(!bay.ship)
		{
			bay.ship = ship;
			ship->SetSystem(nullptr);
			ship->SetPlanet(nullptr);
			return true;
		}
	return false;
}



void Ship::UnloadFighters()
{
	for(Bay &bay : fighterBays)
		if(bay.ship)
		{
			bay.ship->SetSystem(currentSystem);
			bay.ship->SetPlanet(landingPlanet);
			bay.ship.reset();
		}
	for(Bay &bay : droneBays)
		if(bay.ship)
		{
			bay.ship->SetSystem(currentSystem);
			bay.ship->SetPlanet(landingPlanet);
			bay.ship.reset();
		}
}



bool Ship::IsFighter() const
{
	const string &category = attributes.Category();
	return (category == "Fighter" || category == "Drone");
}



bool Ship::HasBays() const
{
	return !droneBays.empty() || !fighterBays.empty();
}



vector<shared_ptr<Ship>> Ship::CarriedShips() const
{
	vector<shared_ptr<Ship>> ships;
	for(const Bay &bay : fighterBays)
		if(bay.ship)
			ships.push_back(bay.ship);
	for(const Bay &bay : droneBays)
		if(bay.ship)
			ships.push_back(bay.ship);
	return ships;
}



CargoHold &Ship::Cargo()
{
	return cargo;
}




const CargoHold &Ship::Cargo() const
{
	return cargo;
}



// Get outfit information.
const map<const Outfit *, int> &Ship::Outfits() const
{
	return outfits;
}



int Ship::OutfitCount(const Outfit *outfit) const
{
	auto it = outfits.find(outfit);
	return (it == outfits.end()) ? 0 : it->second;
}



const Outfit &Ship::Attributes() const
{
	return attributes;
}




const Outfit &Ship::BaseAttributes() const
{
	return baseAttributes;
}



// Add or remove outfits. (To remove, pass a negative number.)
void Ship::AddOutfit(const Outfit *outfit, int count)
{
	if(outfit && count)
	{
		auto it = outfits.find(outfit);
		if(it == outfits.end())
			outfits[outfit] = count;
		else
		{
			it->second += count;
			if(!it->second)
				outfits.erase(it);
		}
		attributes.Add(*outfit, count);
		if(outfit->IsWeapon())
			armament.Add(outfit, count);
		
		if(outfit->Get("cargo space"))
			cargo.SetSize(attributes.Get("cargo space"));
	}
}



// Get the list of weapons.
Armament &Ship::GetArmament()
{
	return armament;
}



const vector<Armament::Weapon> &Ship::Weapons() const
{
	return armament.Get();
}



// Check if we are able to fire the given weapon (i.e. there is enough
// energy, ammo, and fuel to fire it).
bool Ship::CanFire(const Outfit *outfit) const
{
	if(!outfit || !outfit->IsWeapon())
		return false;
	
	if(outfit->Ammo())
	{
		auto it = outfits.find(outfit->Ammo());
		if(it == outfits.end() || it->second <= 0)
			return false;
	}
	
	if(energy < outfit->WeaponGet("firing energy"))
		return false;
	if(fuel < outfit->WeaponGet("firing fuel"))
		return false;
	
	return true;
}



// Fire the given weapon (i.e. deduct whatever energy, ammo, or fuel it uses
// and add whatever heat it generates. Assume that CanFire() is true.
void Ship::ExpendAmmo(const Outfit *outfit)
{
	if(!outfit)
		return;
	if(outfit->Ammo())
		AddOutfit(outfit->Ammo(), -1);
	
	energy -= outfit->WeaponGet("firing energy");
	fuel -= outfit->WeaponGet("firing fuel");
	heat += outfit->WeaponGet("firing heat");
}



bool Ship::CannotAct() const
{
	return (zoom != 1. || isDisabled || hyperspaceCount || pilotError || cloak);
}



double Ship::MinimumHull() const
{
	double maximumHull = attributes.Get("hull");
	return max(.20 * maximumHull, min(.50 * maximumHull, 400.));
}



void Ship::CreateExplosion(list<Effect> &effects)
{
	if(sprite.IsEmpty() || !sprite.GetMask(0).IsLoaded() || explosionEffects.empty())
		return;
	
	// Bail out if this loops enough times, just in case.
	for(int i = 0; i < 10; ++i)
	{
		Point point((Random::Real() - .5) * .5 * sprite.Width(),
			(Random::Real() - .5) * .5 * sprite.Height());
		if(sprite.GetMask(0).Contains(point, Angle()))
		{
			// Pick an explosion.
			int type = Random::Int(explosionTotal);
			auto it = explosionEffects.begin();
			for( ; it != explosionEffects.end(); ++it)
			{
				type -= it->second;
				if(type < 0)
					break;
			}
			effects.push_back(*it->first);
			effects.back().Place(angle.Rotate(point) + position, velocity, angle);
			++explosionCount;
			return;
		}
	}
}



// Each ship can have a target system (to travel to), a target planet (to
// land on) and a target ship (to move to, and attack if hostile).
shared_ptr<Ship> Ship::GetTargetShip() const
{
	return targetShip.lock();
}



shared_ptr<Ship> Ship::GetShipToAssist() const
{
	return shipToAssist.lock();
}



const StellarObject *Ship::GetTargetPlanet() const
{
	return targetPlanet;
}



const System *Ship::GetTargetSystem() const
{
	return targetSystem;
}



const Planet *Ship::GetDestination() const
{
	return destination;
}



// Set this ship's targets.
void Ship::SetTargetShip(const weak_ptr<Ship> &ship)
{
	targetShip = ship;
}



void Ship::SetShipToAssist(const weak_ptr<Ship> &ship)
{
	shipToAssist = ship;
}




void Ship::SetTargetPlanet(const StellarObject *object)
{
	targetPlanet = object;
}


void Ship::SetTargetSystem(const System *system)
{
	targetSystem = system;
}



void Ship::SetDestination(const Planet *planet)
{
	destination = planet;
}



// Add escorts to this ship. Escorts look to the parent ship for movement
// cues and try to stay with it when it lands or goes into hyperspace.
void Ship::AddEscort(const weak_ptr<Ship> &ship)
{
	escorts.push_back(ship);
}



void Ship::SetParent(const weak_ptr<Ship> &ship)
{
	parent = ship;
	targetShip.reset();
	targetPlanet = nullptr;
	targetSystem = nullptr;
}



void Ship::RemoveEscort(const Ship *ship)
{
	auto it = escorts.begin();
	for( ; it != escorts.end(); ++it)
		if(it->lock().get() == ship)
		{
			escorts.erase(it);
			return;
		}
}



void Ship::ClearEscorts()
{
	escorts.clear();
}



const vector<weak_ptr<Ship>> &Ship::GetEscorts() const
{
	return escorts;
}



shared_ptr<Ship> Ship::GetParent() const
{
	return parent.lock();
}
