// Pinscape Pico - Micro Pin Sim
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// An extremely simple miniature pinball physics simulator

#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <list>
#include <vector>
#include <functional>
#include <Windows.h>
#include "WInUtil.h"
#include "MicroPinSim.h"

// debug only
#ifdef _DEBUG
#include <stdio.h>
#include <Windows.h>
void DbgOut(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);

	va_list va2;
	va_copy(va2, va);
	int len = _vscprintf(fmt, va2);
	va_end(va2);

	std::vector<char> buf;
	buf.resize(len + 1);
	vsprintf_s(buf.data(), len + 1, fmt, va);
	OutputDebugStringA(buf.data());
}
#else
#define DbgOut(...)
#endif

using namespace MicroPinSim;

// --------------------------------------------------------------------------
//
// Point
//

Point Point::operator+(const Vec2 &v) const { return Point(x + v.x, y + v.y); }
Point Point::operator-(const Vec2 &v) const { return Point(x - v.x, y - v.y); }
void Point::Add(const Vec2 &v) { x += v.x; y += v.y; }
void Point::Sub(const Vec2 &v) { x -= v.x; y -= v.y; }

// --------------------------------------------------------------------------
//
// Table
//

Table::Table()
{
}

void Table::SetDebugMode(bool enable)
{
	debugMode = enable;
	if (!debugMode)
	{
		for (auto &f : flippers)
			f.departureAngle.clear();
	}
}

void Table::SetUndoCapture(bool enable)
{
	captureUndo = enable;
	if (!enable)
		undo.clear();
}

void Table::UndoEvolve()
{
	if (undo.size() != 0)
	{
		undo.back().Apply(evolveState);
		undo.pop_back();
	}
}

void Table::Add(Element *ele)
{
	// add it as a drawing element
	elements.emplace_back(ele);

	// add it as a collidable, if applicable
	if (auto *c = dynamic_cast<Collidable*>(ele); c != nullptr)
		collidable.collidable.emplace_back(c);

	// add it as a moveable, if applicable
	if (auto *m = dynamic_cast<Moveable*>(ele); m != nullptr)
		moveable.emplace_back(m);
}

// Minimum reversal time for collision detection
static const float MIN_REVERSAL_TIME = 1.0e-6f;

// Minimum convergence speed for collision detection
static const float MIN_CONVERGENCE_SPEED = 1.0e-5f;

// Set flipper button states
void Table::SetFlipperButtons(bool left, bool right)
{
	// set the energized state to the left or right button state
	for (auto &f : flippers)
		f.energized = (f.leftButton ? left : right);
}

// set accelerometer input from integrated velocity readings
void Table::SetNudgeVelocity(float x, float y)
{
	// Set the new live device state.  The accelerometer reading
	// represents the motion of the *table*, which in our simulation
	// model defines the coordinate system that everything else moves
	// relative to.  So if the table moves in one direction, all of
	// the inertial simulation objects move the opposite direction. 
	// The nudge velocity is applied to the inertial objects, so
	// reverse the sign.
	nudgeVelocityState.vDevice ={ -x, -y };
}

// update nudge velocity state for a time step
Vec2 Table::NudgeVelocityState::Update()
{
	// note the 'before' velocity
	Vec2 v0 = vModel;

	// update the model with the live device state
	vModel = vDevice;

	// return the difference between the new and old model velocities
	return vModel - v0;
}

// set accelerometer input from raw acceleration readings
void Table::SetNudgeAccel(float x, float y)
{
	// Set the new instantaneous acceleration.  The input is in 'g'
	// units, equal to 9806.65 mm/s^2.  Convert to mm/s^2.  Reverse
	// the sign, since the acceleration applies to the table, and
	// the table acts as the coordinate system; so moving the table
	// by +X,+Y has the effect of moving all of the objects by -X,-Y.
	nudgeAccelState.a ={ x * -9806.65f, y * -9806.65f };
}

// update accelerometer state for a time step
Vec2 Table::NudgeAccelState::Update(float dt)
{
	// note the 'before' velocity
	Vec2 v0 = v;

	// decay the velocity towards zero
	v = v * decayFactor;

	// add the acceleration into the cumulative nudge velocity
	v = v + (a * dt);

	// return the difference in velocity
	return v - v0;
}

// Evolve the table model by a time step
void Table::Evolve(float dt)
{
	// set the time step
	auto &state = evolveState;
	state.dt = dt;

	// process until we finish the time step or reach an exit condition
	for (;;)
	{
		switch (state.phase)
		{
		case EvolveState::Phase::Move:
			// start of time step - move all moveable objects at their current speeds
			{
				// move everything that can move, by the time step
				for (auto &m : moveable)
					m->Move(state.dt);

				// allow the whole time step for collision search time reversal
				state.dtReversalMax = state.dt;

				// reset the maximum collisions for the step
				state.remainingCollisions = 100;

				// advance to Collision Search phase
				state.phase = EvolveState::Phase::CollisionSearch;
			}
			break;

		case EvolveState::Phase::CollisionSearch:
			// Search for the next collision
			if (state.remainingCollisions-- > 0)
			{
				// Search for the earliest collision with non-zero overlap
				auto &firstColl = state.collision;
				firstColl.Clear();
				for (auto &b : balls)
				{
					for (auto &c : collidable.collidable)
					{
						// Test the collision.  If it has a positive overlap reversal
						// time, and it's the longest such time so far, note it as
						// the earliest collision so far.  If the reversal time is
						// negative, there's no collision at all, so ignore it.  If
						// the reversal time is zero, executing the collision wouldn't
						// take the objects out of collision with each other, so we'd
						// get stuck - ignore these, so that we can pick them up on
						// the next time step where they've advanced far enough on
						// their trajectories that they're in overlap.  Set a slight
						// minimum above zero to reduce the chances that we'll get
						// stuck in a still-in-contact situation due by underflowing
						// the precision of the floats.
						std::unique_ptr<Collidable::Context> ctx;
						if (float t = c->TestCollision(b, ctx, state.dtReversalMax); t > MIN_REVERSAL_TIME && t > firstColl.t)
							firstColl.Set(t, &b, c, ctx);
					}
				}

				// if we didn't find anything in collision, we're done with the
				// collision search
				if (firstColl.b == nullptr)
				{
					state.phase = EvolveState::Phase::Accelerate;
					break;
				}

				// capture undo if desired
				if (captureUndo)
				{
					auto &group = undo.emplace_back(state);
					for (auto &m : moveable)
						group.undo.emplace_back(m->SaveUndo());
				}

				// Back out the times of the ball and collidable object to exactly
				// the point of first contact
				firstColl.b->Move(-firstColl.t);
				firstColl.c->CollisionMove(-firstColl.t, firstColl.ctx.get());

				// switch to collision processing mode
				state.phase = EvolveState::Phase::CollisionExec;

				// If we're in collision-step mode, and we're in the Search phase,
				// stop here so that the caller can update the UI at this position.
				if (state.collisionStepMode)
					return;
			}
			else
			{
				// out of collisions for this round - end the time step
				state.phase = EvolveState::Phase::Accelerate;
			}
			break;

		case EvolveState::Phase::CollisionExec:
			// Execute the collision
			{
				auto &firstColl = state.collision;
				firstColl.c->ExecuteCollision(*firstColl.b, firstColl.ctx.get(), firstColl.t);

				// Now move the simulation forward again from the collision point,
				// with the objects on their new trajectories
				firstColl.b->Move(firstColl.t);
				firstColl.c->CollisionMove(firstColl.t, firstColl.ctx.get());

				// cap the next time reversal at the last one
				state.dtReversalMax = firstColl.t;

				// return to collision search phase
				state.phase = EvolveState::Phase::CollisionSearch;
			}
			break;

		case EvolveState::Phase::Accelerate:
			// End of time step - update accelerations
			{
				// figure the nudge velocity contribution from the acceleration and
				// velocity sensor inputs
				Vec2 vNudge = nudgeAccelState.Update(dt) + nudgeVelocityState.Update();

				// apply accelerations
				for (auto &m : moveable)
				{
					// add nudge velocity
					m->Nudge(vNudge);

					// accelerate
					m->Accelerate(state.dt);
				}

				// reset to start of time step and end the step
				state.phase = EvolveState::Phase::Move;
				return;
			}
		}
	}
}

void Table::Draw(DrawingCtx &ctx, const std::list<Ball> &balls, const std::list<Flipper> &flippers) const
{
	// draw the outer frame
	FrameRect(ctx.hdc, &ctx.rc, HBrush(HRGB(0x404040)));

	// select a black pen for drawing walls and borders
	HPen blackPen(HRGB(0x000000), 2);
	HPEN oldPen = SelectPen(ctx.hdc, blackPen);

	// draw everything
	for (auto *ele : elements)
		ele->Draw(ctx);

	// restore the HDC
	SelectPen(ctx.hdc, oldPen);
}

// --------------------------------------------------------------------------
// 
// Collidable List
//

float CollidableList::TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const
{
	struct
	{
		float t = -1.0f;
		Collidable *c = nullptr;
		std::unique_ptr<Collidable::Context> subCtx;

		void Set(float t, Collidable *c, std::unique_ptr<Collidable::Context> &subCtx)
		{
			this->t = t;
			this->c = c;
			this->subCtx.reset(subCtx.release());
		}
	} firstColl;
	for (auto &c : collidable)
	{
		// Test the collision.  If it has a positive overlap reversal
		// time, and it's the longest such time so far, note it as
		// the earliest collision so far.  If the reversal time is
		// negative, there's no collision at all, so ignore it.  If
		// the reversal time is zero, executing the collision wouldn't
		// take the objects out of collision with each other, so we'd
		// get stuck - ignore these, so that we can pick them up on
		// the next time step where they've advanced far enough on
		// their trajectories that they're in overlap.  Set a slight
		// minimum above zero to reduce the chances that we'll get
		// stuck in a still-in-contact situation due by underflowing
		// the precision of the floats.
		std::unique_ptr<Collidable::Context> subCtx;
		if (float t = c->TestCollision(ball, subCtx, dtMax); t > MIN_REVERSAL_TIME && t > firstColl.t)
			firstColl.Set(t, c, subCtx);
	}

	// if we found a collision, return it
	if (firstColl.t > 0.0f)
	{
		ctx.reset(new CollCtx(firstColl.c, firstColl.subCtx.release()));
		return firstColl.t;
	}

	// no collision detected
	return -1.0f;
}

void CollidableList::ExecuteCollision(Ball &ball, Context *ctx0, float dtRev)
{
	auto *ctx = static_cast<CollCtx*>(ctx0);
	ctx->c->ExecuteCollision(ball, ctx->subCtx.get(), dtRev);
}

void CollidableList::CollisionMove(float dt, Context *ctx0)
{
	auto *ctx = static_cast<CollCtx*>(ctx0);
	ctx->c->CollisionMove(dt, ctx->subCtx.get());
}

// --------------------------------------------------------------------------
//
// Round stationary object
//

float Round::TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const
{
	// figure the overlap distance
	float overlap = ball.r + r - c.Dist(ball.c);
	if (overlap < 0.0f)
		return -1.0f;

	// figure the speed of the ball towards the center
	float speed = Vec2(ball.c, c).Unit().Dot(ball.v);

	// figure the time since contact - the amount of time it takes
	// at the convergence speed to cover the overlap distance
	return overlap / speed;
}

void Round::ExecuteCollision(Ball &ball, Context*, float)
{
	// reverse the ball's velocity along the convergence vector
	Vec2 n = Vec2(ball.c, c).Unit();
	ball.v = ball.v - n * (1.0f + e) * (ball.v.Dot(n));
}


// --------------------------------------------------------------------------
//
// Line segment
//

float LineSeg::TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const
{
	// check for a collision somewhere along the wall proper
	float t = Project(ball.c);
	if (t > 0.0f && t < 1.0f)
	{
		// Collision along the wall proper - figure the point of collision
		// P by interpolating the nearest point along the segment, and
		// the vector from there to the ball center.  The distance is the
		// length of the vector.
		auto p2c = Vec2(ball.c, Interpolate(t));
		float d = p2c.Mag();
		if (d <= ball.r)
		{
			// figure the convergence speed - the balls speed along the 
			// vector from ball to collision point
			float speed = p2c.Unit().Dot(ball.v);

			// the reversal time is the time it takes to cover the overlap
			// distance at the convergence speed
			return (ball.r - d) / speed;
		}
	}

	// check for a collision with the first endpoint
	float d = ball.c.Dist(p1);
	if (d <= ball.r)
	{
		float speed = Vec2(ball.c, p1).Unit().Dot(ball.v);
		return (ball.r - d) / speed;
	}

	// check the second endpoint
	d = ball.c.Dist(p2);
	if (d <= ball.r)
	{
		float speed = Vec2(ball.c, p2).Unit().Dot(ball.v);
		return (ball.r - d) / speed;
	}

	// no collision
	return -1.0f;
}

void LineSeg::ExecuteCollision(Ball &ball, Context*, float)
{
	float t = Project(ball.c);
	if (t < 0.0f)
	{
		// collision with endpoint 1
		Vec2 n = Vec2(ball.c, p1).Unit();
		ball.v = ball.v - n * (1.0f + e) * (ball.v.Dot(n));
	}
	else if (t > 1.0f)
	{
		// collision with endpoint 2
		Vec2 n = Vec2(ball.c, p2).Unit();
		ball.v = ball.v - n * (1.0f + e) * (ball.v.Dot(n));
	}
	else
	{
		// collision along the line segment, at projected offset t

		// get the reflection vector - this is the unit vector normal
		// to the line segment on the same side as the ball, which is
		// also simply the vector from contact point to the ball center
		Vec2 n = Vec2(Interpolate(t), ball.c).Unit();

		// update the velocity
		ball.v.Sub(n * (1.0f + e) * (ball.v.Dot(n)));
	}
}

// --------------------------------------------------------------------------
//
// Polygon
//

Polygon::Polygon(float r, float e, const std::list<float> &points) : r(r), e(e)
{
	std::list<Point> points2;
	for (auto it = points.begin() ; it != points.end() ; )
	{
		float x = *it++;
		float y = *it++;
		points2.emplace_back(x, y);
	}
	Init(points2);
}

Polygon::Polygon(float r, float e, const std::list<Point> &points) : r(r), e(e)
{
	Init(points);
}

void Polygon::Init(const std::list<Point> &points)
{
	const Point *ptPrv = &points.back();
	for (const auto &pt : points)
	{
		// if we have more than one vertex, add line segments between vertices
		if (points.size() > 1)
		{
			// check the radius
			if (r == 0)
			{
				// zero radius - this is a wall, so just add line segments,
				// without closing the polygon at the last element
				if (&pt != &points.front())
					collidable.emplace_back(&edges.emplace_back(*ptPrv, pt, e));
			}
			else
			{
				// non-zero radius - treat it as a closed polygon with
				// rounded corners, with the vertices arranged clockwise

				// add the vertex as a Round
				collidable.emplace_back(&vertices.emplace_back(pt, r, e));

				// construct a line segment between the previous point and this one
				LineSeg l(*ptPrv, pt, e);

					// get a normal, rotated 90 degrees counter-clockwise, of length 
				// equal to the corner radius
				Vec2 v(Vec2(l.p2 - l.p1).Unit() * r);
				Vec2 n(-v.y, v.x);

				// push the new line segment along the normal for the radius, to
				// make it tangent to the two corner circles
				l.p1 = l.p1 + n;
				l.p2 = l.p2 + n;

				// add the line segment to the edge list
				collidable.emplace_back(&edges.emplace_back(l));
			}
		}
		else
		{
			// single round object
			collidable.emplace_back(&vertices.emplace_back(pt, r, e));
		}

		// remember the previous vertex
		ptPrv = &pt;
	}
}

void Polygon::Draw(DrawingCtx &ctx) const
{
	// draw circles at the vertices, representing posts
	for (const auto &v : vertices)
		ctx.DrawCircle(v.c, r);

	// draw the edges
	for (const auto &e : edges)
		ctx.DrawLine(e);
}

// --------------------------------------------------------------------------
//
// Ball
//

void Ball::Draw(DrawingCtx &ctx) const
{
	ctx.DrawCircle(c, r);
	if (ctx.dataMode)
	{
		HPen redPen(HRGB(0xff0000));
		HPEN oldPen = SelectPen(ctx.hdc, redPen);
		POINT sc = ctx.Pt(c);
		POINT cv = ctx.Pt(c + (v * 0.1f));
		MoveToEx(ctx.hdc, sc.x, sc.y, NULL);
		LineTo(ctx.hdc, cv.x, cv.y);
		SetBkMode(ctx.hdc, OPAQUE);
		SetBkColor(ctx.hdc, HRGB(0xffffff));
		int x = sc.x + static_cast<int>(r*ctx.scale) + 2;
		int y = sc.y - ctx.tmDataFont.tmHeight;
		y += ctx.hdc.DrawTextF(x, y, 1, ctx.dataFont, HRGB(0xFF0000), "%.2f,%.2f", c.x, c.y).cy;
		y += ctx.hdc.DrawTextF(x, y, 1, ctx.dataFont, HRGB(0xFF0000), "v=%.2f,%.2f", v.x, v.y).cy;
		SelectPen(ctx.hdc, oldPen);
		SetBkMode(ctx.hdc, TRANSPARENT);
	}
}

void Ball::Accelerate(float dt)
{
	// Apply gravity if enabled
	if (table->IsGravityEnabled())
	{
		// sin(slope) * 9.8m/s^2 * dt, slope = 6 degrees
		v.y -= 0.104528f * 9806.65f * dt;
	}

	// deduct a bit of friction
	v = v * 0.9998f;
}

float Ball::TestCollision(const Ball &b, std::unique_ptr<Context> &ctx, float dtMax) const
{
	// we can't collide with ourself
	if (&b == this)
		return -1.0f;

	// Figure the distance between the centers.  If it's greater than
	// the sum of the radii, the balls aren't in contact.
	float overlap = (r + b.r) - c.Dist(b.c);
	if (overlap < 0.0f)
		return -1.0f;

	// They're in contact.  Figure out their relative velocity along
	// the vector between the two centers.
	Vec2 vcc = Vec2(c, b.c).Unit();
	float vRel = vcc.Dot(v) - vcc.Dot(b.v);

	// If it's very small, we're in rounding-error territory, so consider
	// it a miss.  (In particular, dividing by the velocity to get the backup
	// time can blow up into an infinity.)  This will allow the balls to 
	// continue on their trajectories for another time step, at which point
	// they'll either have enough overlap to calculate the collision
	// correctly, or they'll have passed through each other.  Passing
	// through each other isn't good, but it's better than an infinity.
	if (vRel <= MIN_CONVERGENCE_SPEED)
		return -1.0f;

	// Now figure out how long they've been in contact, as the time
	// it takes to cover the overlap distance at the relative velocity.
	return overlap / vRel;
}

void Ball::ExecuteCollision(Ball &b, Context*, float)
{
	// figure the collision outcome using the canonical billiard ball formula
	// for elastic collisions
	Vec2 x1 = Vec2(c);
	Vec2 x2 = Vec2(b.c);
	Vec2 dv1 = (x1 - x2) * ((2.0f * b.m / (m + b.m)) * ((v - b.v).Dot(x1 - x2)) / ((x1 - x2).MagSquared()));
	Vec2 dv2 = (x2 - x1) * ((2.0f * m / (m + b.m)) * ((b.v - v).Dot(x2 - x1)) / ((x2 - x1).MagSquared()));

	v.Sub(dv1);
	b.v.Sub(dv2);
}

// --------------------------------------------------------------------------
//
// Flipper
//


Flipper::Flipper(Table *table, bool leftButton, float x, float y, float len, float r1, float r2, float restAngle, float flipAngle, float e) :
	table(table), leftButton(leftButton), e(e), cr(x, y), len(len), r1(r1), r2(r2), 
	thetaDown(restAngle * 3.14159265f/180.0f), thetaUp((restAngle + flipAngle) * 3.14159265f/180.0f),
	topEdge(this), bottomEdge(this), round1(x, y, r1, e), round2(this, x, y, r2)
{
	// add the collidable components to the list
	collidable.emplace_back(&topEdge);
	collidable.emplace_back(&bottomEdge);
	collidable.emplace_back(&round1);
	collidable.emplace_back(&round2);

	// set the edge surface normal turn directions
	if (thetaUp > thetaDown)
	{
		// counter-clockwise rotation -> left-side flipper
		topEdge.normalDir = 1.0f;
		bottomEdge.normalDir = -1.0f;
	}
	else
	{
		// clockwise rotation -> right-side flipper
		topEdge.normalDir = -1.0f;
		bottomEdge.normalDir = 1.0f;
	}


	// figure the edge radius angle
	if (r1 == r2)
	{
		// same diameter - the edges are parallel to the center line,
		// so the radii to the edges are at right angles to the center line
		edgeRadiusAngle = 3.14159265f/2.0f;
	}
	else
	{
		// different diameters - the edges run at an angle to the center line
		float x = len * r1 / (r2 - r1);
		edgeRadiusAngle = acosf(r1 / x);
	}

	// estimate maximum angular speed
	omegaMax = sqrtf(2.0f * fabsf(thetaUp - thetaDown) * alphaLift);

	// set initially to the resting ("down") position
	SetTheta(thetaDown);
}

void Flipper::Draw(DrawingCtx &ctx) const
{
	ctx.DrawCircle(cr, r1);
	ctx.DrawCircle(round2.c, r2);
	ctx.DrawLine(topEdge);
	ctx.DrawLine(bottomEdge);

	if (ctx.dataMode)
	{
		// label the angle and speed
		HPen redPen(HRGB(0xff0000));
		HPEN oldPen = SelectPen(ctx.hdc, redPen);
		POINT sc = ctx.Pt(cr);
		SetBkMode(ctx.hdc, OPAQUE);
		SetBkColor(ctx.hdc, HRGB(0xffffff));
		int dir = thetaDown > 3.14159265f/2.0f ? 1 : -1;
		int x = sc.x + dir*(static_cast<int>(r1*ctx.scale) + 2);
		int y = sc.y - ctx.tmDataFont.tmHeight;
		y += ctx.hdc.DrawTextF(x, y, dir, ctx.dataFont, HRGB(0xFF0000), "th=%.2f", theta*180.0f/3.14159265f).cy;
		y += ctx.hdc.DrawTextF(x, y, dir, ctx.dataFont, HRGB(0xFF0000), "w=%.2f", omega*180.0f/3.14159265f).cy;
		SetBkMode(ctx.hdc, TRANSPARENT);

		// draw departure vectors
		HPen ltRedPen(HRGB(0xffC0C0));
		SelectPen(ctx.hdc, ltRedPen);
		for (const auto &da : departureAngle)
		{
			if (da.phase == 1)
			{
				auto p1 = ctx.Pt(da.ptActuate);
				auto p2 = ctx.Pt(da.ptFinal);
				MoveToEx(ctx.hdc, p1.x, p1.y, NULL);
				LineTo(ctx.hdc, p2.x, p2.y);
			}
		}
		SelectPen(ctx.hdc, oldPen);
	}
}

void Flipper::Move(float dt)
{
	// reduce the top-of-arc time window remaining
	static const float topStopHangoverTimeSteps = 120.0f;
	if (topStopTime != 0.0f)
	{
		topStopTime = fmaxf(0.0f, topStopTime - dt);
		static const float topStopOmegaDecay = pow(0.5f, 2.0f/topStopHangoverTimeSteps);
		topStopOmega *= topStopOmegaDecay;
	}
	
	// adjust speed
	if (omega != 0.0f)
	{
		// move by the angular velocity
		float thetaNew = theta + (omega * dt);

		// check if we're past the stops
		float pos = (theta - thetaDown) / (thetaUp - thetaDown);
		if (pos > 1.0f)
		{
			// Past the top stop.  Peg it to the top stop and stop the flipper.
			// A real flipper assembly overshoots the stop stop by a measurable
			// distance for a measurable amount of time (it's about 8 degrees
			// for 4-5ms in the ones I've observed), but it's too fast to see
			// to the unaided eye, and I haven't found that it improves the
			// collision physics to model it, so it's easier to just pretend
			// that the flipper stops cold.
			thetaNew = thetaUp;

			// If it's moving near the maximum speed for an unloaded flip,
			// start the special time window representing sudden deceleration
			// upon hitting the top stop.  This changes the collision physics
			// slightly to account for the reality that the flipper's stop
			// time isn't perfectly instantaneously.
			if (fabsf(omega) >= omegaMax * 0.7f)
				topStopTime = dt * topStopHangoverTimeSteps;

			// dead stop
			omega = 0.0f;
		}
		else if (pos < 0.0f)
		{
			// Past bottom stop.  A real flipper has a little bit of bounce
			// at the stop that's slow enough to see by eye, so peg the flipper
			// to the stop, and reverse the speed with damping.  Stop entirely
			// when it's below a threshold.
			thetaNew = thetaDown;
			omega = (fabs(omega) < 0.08f) ? 0.0f : -omega * 0.4f;
		}

		// adjust the position if it changed
		if (thetaNew != theta)
			SetTheta(thetaNew);
	}
}

void Flipper::Accelerate(float dt)
{
	// figure the stroke position, on a scale from 0 (at rest) to 1 (at the top stop)
	float pos = (theta - thetaDown) / (thetaUp - thetaDown);

	// figure the sign on alpha
	float alphaSign = (thetaUp > thetaDown) ? 1.0f : -1.0f;

	// Figure the acceleration.  If the coil is energized, apply
	// the solenoid lift or hold force.  Otherwise apply the spring
	// return force.
	float alpha = 0.0f;
	if (energized && theta != thetaUp)
	{
		// Energized - apply the lift or hold coil force, or nothing if we're at the stop
		alpha = pos < eos ? alphaLift : pos < 1.0f ? alphaHold : 0.0f;
	}
	else if (theta != thetaDown)
	{
		// Spring return force
		alpha = -alphaSpring;
	}

	if (table->IsDebugMode())
	{
		if (pos < 0.005f && energized && (departureAngle.size() == 0 || departureAngle.back().phase != 0))
		{
			// check if the first ball is close to the flipper
			auto *ball = table->FirstBall();
			float t = topEdge.Project(ball->c);
			if (t >= 0.0f && t <= 1.0f && topEdge.Interpolate(t).Dist(ball->c) < ball->r + 3.0f)
			{
				// start a new record
				departureAngle.emplace_back();
				departureAngle.back().ptActuate = ball->c;
			}
		}
		if (departureAngle.size() != 0)
		{
			auto &da = departureAngle.back();
			if (da.phase == 0)
			{
				da.dt += dt;
				auto *ball = table->FirstBall();
				if (ball->c.y > 400.0f)
				{
					da.phase = 1;
					da.ptFinal = ball->c;
				}
				else if (da.dt > 1.0f)
					departureAngle.pop_back();
			}
		}
	}

	// increase the angular velocity by the acceleration over the time step
	omega += dt * alpha * alphaSign;
}

void Flipper::SetTheta(float thetaNew)
{
	// set the new theta
	theta = thetaNew;

	// set the moving end position
	round2.c = cr + Vec2::FromPolar(len, theta);

	// calculate the edge segments
	CalcEdge(theta, &topEdge, topEdge.p1, topEdge.p2);
	CalcEdge(theta, &bottomEdge, bottomEdge.p1, bottomEdge.p2);
}

void Flipper::CalcEdge(float theta, const FlipperEdge *edge, Point &p1, Point &p2) const
{
	// figure the radius angle, based on which edge we're talking about
	float radiusAngle = (edge == &topEdge ? thetaUp > thetaDown : thetaUp < thetaDown) ? theta + edgeRadiusAngle : theta - edgeRadiusAngle;

	// figure the endpoints; the edge is the line between the two tangent
	// points on the end circles at the radius angle from the center line
	p1 = cr + Vec2::FromPolar(r1, radiusAngle);
	p2 = round2.c + Vec2::FromPolar(r2, radiusAngle);
}

float Flipper::CalcContactDistance(float theta, const FlipperEdge *edge, const Point &ballCenter, float ballRadius) const
{
	// set up a line segment representing the edge at the given theta
	LineSeg seg(e);
	CalcEdge(theta, edge, seg.p1, seg.p2);

	// project the endpoint
	float t = seg.Project(ballCenter);
	if (t < 0.0f)
	{
		// it's outside of the line segment at the rotating end - the distance
		// the distance to the rotating end circle
		return Vec2(cr, ballCenter).Mag() - (ballRadius + r1);
	}
	else if (t > 1.0f)
	{
		// it's outside of the line segment at the moving end
		return ballCenter.Dist(cr + Vec2::FromPolar(len, theta)) - (ballRadius + r2);
	}
	else
	{
		// it's along the line - figure the contact point
		return ballCenter.Dist(seg.Interpolate(t));
	}
}

// Root finder.  Finds a root of f() over the interval [a..b].  Initially,
// a and b must satisfy f(a) < 0 and f(b) > 0.
static float FindRoot(std::function<float(float)> f, float a, float b, int maxIterations, float tolerance)
{
	// split the interval
	float c = (a + b)/2.0f;

	// iterate
	for (int i = 0 ; i < maxIterations ; ++i)
	{
		// figure the value at the current midpoint
		float val = f(c);

		// stop when we're within the tolerance
		if (fabsf(val) < tolerance)
			return c;

		// adjust the interval
		if (val < 0.0f)
			a = c;
		else
			b = c;
		c = (a + b)/2.0f;
	}

	// maximum iterations reached - return the current midpoint
	return c;
}

float Flipper::FlipperEdge::TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const
{
	// check for a collision on this edge
	float t = Project(ball.c);
	if (t > 0.0f && t < 1.0f)
	{
		// Figure the point of collision and distance to the ball
		auto contactPoint = Interpolate(t);
		auto p2c = Vec2(ball.c, contactPoint);
		float d = p2c.Mag();
		p2c = p2c * (1.0f/d);
		if (d <= ball.r)
		{
			// figure the speed of the ball along the vector to the collision point
			// (in the playfield rest frame)
			float ballSpeed = p2c.Dot(ball.v);

			// Figure the velocity vector for the contact point on the flipper.
			// All points on the edge move in a circle centered on the flipper's
			// rotation axis, so we need a vector at a right angle to the vector 
			// from rotation center to contact point, in the direction of omega.
			Vec2 r2c(flipper->cr, contactPoint);
			Vec2 vContactPoint = r2c.NormalLeft() * flipper->omega;

			// Figure the convergence speed along the contact surface normal
			float convergenceSpeed = (ball.v - vContactPoint).Dot(p2c);

			// If the convergence speed is negative, they're receding, so it's
			// not a collision.
			if (convergenceSpeed < 0.0f)
				return -1.0f;

			// As a first approximation at the reversal time, treat the convergence
			// speed as constant, so back up by the amount of time it takes to cover
			// the overlap distance.
			float dt = (ball.r - d) / convergenceSpeed;

			// If the flipper is moving, the distance function isn't linear - it's
			// a messy trig function - so the time estimate we just calculated based
			// on linear regression isn't quite right.  I don't know how to solve
			// the function "what is the theta for a given distance?" in closed form,
			// but we can improve on the linear regression estimate by using a root-
			// finder algorithm on the distance(theta) function.  This won't give us
			// the exact answer either, but we can get much closer than the linear
			// estimate.
			if (flipper->omega != 0.0f)
			{
				// figure the distance at the new time value
				auto ContactDistance = [this, &ball](float dt) {
					return flipper->CalcContactDistance(
						flipper->ClampTheta(flipper->theta - dt*flipper->omega), this, ball.c - (ball.v * dt), ball.r) - ball.r;

				};
				float dNew = ContactDistance(dt);

				// Figure the search interval endpoints.  If the new distance is
				// less than the ball radius, we haven't gone back far enough yet
				// to remove the overlap, so search from 'dt' to the maximum step
				// time.  Otherwise, we've gone back too far, so search from 0
				// to dt.
				float dt0, dt1;
				if (dNew < 0.0f)
				{
					// still in contact -> dt isn't back far enough
					dt0 = dt;
					dt1 = dtMax;

					// make sure that end1 is actually out of contact; if not,
					// stop here with the maximum reversal time
					if (ContactDistance(dt1) < 0.0f)
						return dt1;
				}
				else
				{
					// dt is too far
					dt0 = 0.0f;
					dt1 = dt;
				}

				// find the contact point within dt0..dt1
				dt = FindRoot(ContactDistance, dt0, dt1, 10, 1.0e-5f);

				// limit the backup time to dtMax
				dt = fmaxf(dt, dtMax);
			}

			// report the reversal time
			return dt;
		}
	}

	// no collision
	return -1.0f;
}

void Flipper::FlipperEdge::ExecuteCollision(Ball &ball, Context *ctx, float dtRev)
{
	// get the contact point
	float t = Project(ball.c);
	auto contactPoint = Interpolate(t);
	auto rotationCenterToContact = Vec2(flipper->cr, contactPoint);

	// Get the unit vector from contact point to ball.  Since this is normal
	// to the flipper edge, it's also the collision normal.
	auto p2c = Vec2(contactPoint, ball.c);
	auto p2cDist = p2c.Mag();
	auto surfaceNormal = p2c * (1.0f / p2cDist);

	// If the flipper is currently in the down position, and we're hitting
	// the top side, or it's in the up position, and we're hitting the bottom
	// side, it can't rotate at all from here, so the collision is just an
	// elastic collision with an immovable object.
	if (flipper->IsAtStop(this))
	{
		// at the stop - it's a simple elastic collision with an immovable flipper
		ball.v.Sub(surfaceNormal * ((1.0f + e) * ball.v.Dot(surfaceNormal)));
	}
	else
	{
		// This flipper isn't being driven into a stop, so the collision energy
		// is split between the ball's linear motion and the flipper rotational
		// motion.

		// Figure the transfer coefficient.  This is based on the classic elastic
		// collision formula for linear plus angular velocity, but modified for the
		// special arrangement where the flipper is fixed in place at its center of
		// rotation.  The flipper's motion - both before and after the collision - 
		// is constrained to rotation around the shaft, so all of its momentum can
		// be modeled as angular momentum around that point.  The translation between
		// the flipper's rotational motion and the ball's linear motion occurs at the
		// point of contact on the edge, and that point moves on a circular arc
		// centered on the flipper shaft.  
		// 
		// In the standard formulation, the momentum transfer is proportional to
		// each object's share of the total mass, but since the flipper's motion is
		// rotational, it's easiest to think about the momentum shares in terms of
		// the relative moments of inertia.  The flipper's moment of inertia is a
		// constant, since it has a fixed mass and rotates about a fixed point.
		// For the ball, we don't think about its rotation around the flipper shaft,
		// since it's motion is linear coming into and exiting the collision, but
		// its *impulse* can be thought of in terms of the angular momentum effect,
		// and in that sense the ball's effective moment of inertia is the lever
		// arm length times the ball's mass.
		float rcb = rotationCenterToContact.Mag();

		// If we're in the special time window when the flipper just hit the
		// top of the arc during a flip, reduce the elasticity, to simulate
		// the deformation of the rubber ring after the sudden deceleration.
		float e = this->e;
		float omega = flipper->omega;
		if (flipper->topStopTime != 0.0f)
		{
			e = e / 2.0f;
			omega = flipper->topStopOmega;
		}

		// figure the relative velocity
		Vec2 vContactPoint = rotationCenterToContact.NormalLeft() * omega;
		Vec2 dv = ball.v - vContactPoint;
		float dvNormal = dv.Dot(surfaceNormal);

		// If they're already receding, or not moving, skip the collision
		if (dvNormal >= 0.0f)
		{
			// Push the ball away from the edge to get it out of contact.
			// We have to make *some* change before returning, because
			// otherwise the collision searcher would find this same
			// non-collision overlap again on the next scan, and we'd
			// get stuck here.
			if (p2cDist < ball.r)
				ball.c = ball.c + (surfaceNormal * (ball.r - p2cDist));
			else
				ball.c = ball.c + (surfaceNormal * 0.5f);

			// skip the rest of the collision processing
			return;
		}

		// figure the transfer coefficient
		float tc = (1.0f + e) * dvNormal / (1.0f/(ball.m * rcb) + 1.0f/flipper->I);

		// figure the momentum transfers along the flipper rotation vector
		Vec2 dvBall = surfaceNormal * (tc / (ball.m * rcb));
		float dwFlipper = tc / (flipper->I * rcb) * normalDir;

		// Add crosswise velocity to the ball when the relative speed is low, to
		// simulate friction.  The friction component seems to be most significant
		// when the relative speed is low, which generally means that the flipper
		// is pushing the ball through a flip rather than the ball coming in and
		// bouncing off the flipper.
		Vec2 edgeUnit = Vec2(p1, p2).Unit();
		float dvCross = ball.v.Dot(edgeUnit) - vContactPoint.Dot(edgeUnit);
		float frictionAdj = fminf(fmaxf(fabsf(dvNormal) - flipper->frictionVMin, 0.0f) / (flipper->frictionVFull - flipper->frictionVMin), 1.0f);
		float dSpeedFriction = (dvCross * flipper->friction * frictionAdj);
		Vec2 dvFriction = edgeUnit * dSpeedFriction;

		// take some additional angular momentum out of the flipper for the friction
		dwFlipper += dSpeedFriction / (flipper->I * rcb) * normalDir;

		// apply the speed changes
		ball.v.Sub(dvBall);
		ball.v.Sub(dvFriction);
		flipper->omega += dwFlipper;
	}
}

float Flipper::FlipperEnd::TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const
{
	// check for a collision
	float overlap = ball.r + r - c.Dist(ball.c);
	if (overlap < 0.0f)
		return -1.0f;

	// figure the speed of the ball towards the center
	float ballSpeed = Vec2(ball.c, c).Unit().Dot(ball.v);

	// figure the contact point
	Vec2 p2c = Vec2(ball.c, c).Unit() * ball.r;
	Point contactPt = ball.c + p2c;

	// Figure the vector from the center of rotation to the contact point,
	// and from there, the velocity vector at the contact point.
	Vec2 r2c(flipper->cr, contactPt);
	Vec2 vCollisionPoint = r2c.NormalLeft().Unit() * flipper->omega;

	// Dot this with the contact-point-to-ball vector to get the speed
	// of this point towards the ball, again in the playfield rest frame
	float contactPointSpeed = -vCollisionPoint.Dot(p2c);

	// Now we can figure the convergence speed, as the sum of the two
	// speeds in the rest frame.
	float convergenceSpeed = ballSpeed + contactPointSpeed;

	// If the convergence speed is positive and not too small, report
	// the collision.
	if (convergenceSpeed < MIN_CONVERGENCE_SPEED)
		return -1.0f;

	// get a first approximation of the reversal time, based on constant
	// convergence speed over the time step
	float dt = overlap / convergenceSpeed;

	// If the flipper is moving, we need to apply a root finder algorithm
	// to the trig function for distance to get a more accurate collision 
	// time estimate.
	if (flipper->omega != 0.0f)
	{
		// figure the distance at the new time value
		auto ContactDistance = [this, &ball](float dt) {
			Point newEnd = flipper->cr + Vec2::FromPolar(flipper->len, flipper->ClampTheta(flipper->theta - dt*flipper->omega));
			return newEnd.Dist(ball.c - (ball.v * dt)) - r - ball.r;
		};
		float dNew = ContactDistance(dt);

		// Figure the search interval endpoints.  If the new distance is
		// less than the ball radius, we haven't gone back far enough yet
		// to remove the overlap, so search from 'dt' to the maximum step
		// time.  Otherwise, we've gone back too far, so search from 0
		// to dt.
		float dt0, dt1;
		if (dNew < 0.0f)
		{
			// still in contact -> dt isn't back far enough
			dt0 = dt;
			dt1 = dtMax;

			// make sure that end1 is actually out of contact; if not,
			// stop here with the maximum reversal time
			if (ContactDistance(dt1) < 0.0f)
				return dt1;
		}
		else
		{
			// dt is too far
			dt0 = 0.0f;
			dt1 = dt;
		}

		// find the point in time of contact
		dt = FindRoot(ContactDistance, dt0, dt1, 10, 1.0e-5f);
	}

	// return the time
	return dt;
}

void Flipper::FlipperEnd::ExecuteCollision(Ball &ball, Context*, float)
{
	// get contact point
	Vec2 p2c = Vec2(c, ball.c).Unit() * r;
	auto p2cUnit = p2c.Unit();
	Point contactPoint = c + p2c;

	// Get the vector from the flipper's center of rotation to the contact point.
	// This is important to figuring the angular momentum transfer.
	auto r2c = Vec2(flipper->cr, contactPoint);

	// Check if the flipper is at a stop in the direction of the impulse
	// from the ball.  If so, treat the flipper as a stationary object
	// rather than a moveable object, since it can't move any further
	// in this direction.  We're effectively colliding the ball against
	// the playfield itself, which we treat as having infinite mass.
	bool atStop = false;
	float impulseAngularDir = (r2c.NormalLeft().Dot(p2c) < 0 ? 1.0f : -1.0f);
	if (impulseAngularDir > 0.0f)
	{
		// the impulse force is counterclockwise
		atStop = (flipper->thetaUp > flipper->thetaDown) ? (flipper->theta >= flipper->thetaUp) : (flipper->theta >= flipper->thetaDown);
	}
	else
	{
		// the impulse force is clockwise
		atStop = (flipper->thetaUp > flipper->thetaDown) ? (flipper->theta <= flipper->thetaDown) : (flipper->theta <= flipper->thetaUp);
	}
	if (atStop)
	{
		// driving into a stop - treat the flipper as stationary
		ball.v.Sub(p2cUnit * ((1.0f + e) * (ball.v.Dot(p2cUnit))));
	}
	else
	{
		// The flipper isn't against a stop, so there's a transfer of energy
		// between the flipper's rotation and the ball's linear velocity.

		// Figure the transfer coefficient.  This is based on the classic elastic
		// collision formula for linear plus angular velocity, treating all of the
		// ball's kinetic energy as coming from its linear velocity and all of the
		// flipper's as rotational.
		float rcb = r2c.Mag();
		if (rcb >= 1.0e-5)
		{
			// figure the relative velocity
			Vec2 vContactPoint = r2c.NormalLeft() * flipper->omega;
			Vec2 dv = ball.v - vContactPoint;

			// if they're already receding, skip the collision
			if (dv.Dot(p2cUnit) > 0.0f)
			{
				// push the ball back to get it out of contact
				float dist = Vec2(contactPoint, ball.c).Mag();
				if (dist < ball.r)
					ball.c = ball.c + (p2cUnit * (ball.r - dist));

				// skip the rest of the collision processing
				return;
			}
				
			// transfer coefficient
			float tc = (1.0f + e) * dv.Dot(p2cUnit) / (1.0f/(ball.m * rcb) + 1.0f/flipper->I);

			// figure the momentum transfers along the flipper rotation vector
			Vec2 dvBall = p2cUnit * (tc / (ball.m * rcb));
			float dwFlipper = tc / flipper->I / r2c.Mag() * -impulseAngularDir;

			// apply the speed changes
			ball.v.Sub(dvBall);
			flipper->omega += dwFlipper;
		}
	}
}

// --------------------------------------------------------------------------
//
// Drawing context
//


// model point to window client coordinates
POINT DrawingCtx::Pt(const Point &pt)
{
	return POINT{
		static_cast<int>(roundf(pt.x * scale + rc.left)),
		static_cast<int>(roundf(rc.bottom - pt.y * scale))
	};
}

// draw a circle
void DrawingCtx::DrawCircle(const Point &pt, float r)
{
	Ellipse(hdc, static_cast<int>(roundf(scale*(pt.x - r) + rc.left)),
		static_cast<int>(roundf(rc.bottom - scale*(pt.y - r))),
		static_cast<int>(roundf(scale*(pt.x + r) + rc.left)),
		static_cast<int>(roundf(rc.bottom - scale*(pt.y + r))));
}

// draw an line
void DrawingCtx::DrawLine(const LineSeg &l)
{
	POINT p1 = Pt(l.p1);
	POINT p2 = Pt(l.p2);
	MoveToEx(hdc, p1.x, p1.y, NULL);
	LineTo(hdc, p2.x, p2.y);
}
