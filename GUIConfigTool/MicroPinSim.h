// Pinscape Pico - Micro Pin Sim
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// An extremely simple miniature pinball physics simulator.  The Pinscape
// Pico Config Tool uses this in the Nudge setup window to provide instant
// viewing of the effects of the accelerometer tuning parameters on a live
// physics model.

#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <list>
#include <memory>
#include <Windows.h>
#include <windowsx.h>
#include "WinUtil.h"

namespace MicroPinSim
{
	// forwards
	struct Point;
	struct Vec2;
	class Ball;
	class LineSeg;
	class Polygon;
	class Table;

	// Drawing context
	struct DrawingCtx
	{
		HDCHelper &hdc;		// target HDC
		RECT rc;			// on-screen drawing area in client coordinates
		float scale;		// scaling factor, model to screen coordinates
		bool dataMode;      // true -> show numeric labels on some objects to show internal model state
		HFONT dataFont;     // data font
		TEXTMETRIC &tmDataFont;  // data font metrics

		// model point to window client coordinates
		POINT Pt(const Point &pt);

		// draw a circle
		void DrawCircle(const Point &pt, float r);

		// draw an line
		void DrawLine(const LineSeg &l);
	};

	// Table element
	class Element
	{
	public:
		virtual void Draw(DrawingCtx &ctx) const = 0;
	};

	// point
	struct Point
	{
		Point() : x(0), y(0) { }
		Point(float x, float y) : x(x), y(y) { }
		Point(const Point &p) : x(p.x), y(p.y) { }

		Point operator +(const Vec2 &v) const;
		Point operator -(const Vec2 &v) const;

		void Add(const Vec2 &v);
		void Sub(const Vec2 &v);

		// distance to another point
		float Dist(const Point &p) const
		{
			float dx = p.x - x;
			float dy = p.y - y;
			return sqrtf(dx*dx + dy*dy);
		}

		float x;
		float y;
	};

	// simple 2D vector
	struct Vec2
	{
		Vec2() : x(0.0f), y(0.0f) { }
		Vec2(float x, float y) : x(x), y(y) { }
		Vec2(const Vec2 &v) : x(v.x), y(v.y) { }
		Vec2(const Point &p) : x(p.x), y(p.y) { }
		Vec2(const Point &a, const Point &b) : x(b.x - a.x), y(b.y - a.y) { }

		static Vec2 FromPolar(float len, float theta) { return Vec2(len * cosf(theta), len * sinf(theta)); }

		Vec2 Unit() const { float m = Mag(); return Vec2(x/m, y/m); }

		Vec2 operator +(const Vec2 &v) const { return Vec2(x + v.x, y + v.y); }
		Vec2 operator -(const Vec2 &v) const { return Vec2(x - v.x, y - v.y); }
		void Add(const Vec2 &v) { x += v.x; y += v.y; }
		void Sub(const Vec2 &v) { x -= v.x; y -= v.y; }

		Vec2 operator *(float f) const { return Vec2(x*f, y*f); } // scale

		// dot product
		float Dot(const Vec2 &v) const { return x*v.x + y*v.y; }

		// Length of three-vector cross product.  This treats the
		// two-vector as a vector is a 3D space with z == 0.  The
		// result of the cross product is a vector in th Z direction
		// of the magnitude we figure here.
		float CrossLen(const Vec2 &v) const { return x*v.y - y*v.x; }

		// project 'b' onto this vector
		Vec2 Project(const Vec2 &a) const {
			const Vec2 &b = *this;
			return b * ((a.Dot(b)) / (b.Dot(b)));
		}

		Vec2 NormalLeft() const { return Vec2(-y, x); }
		Vec2 NormalRight() const { return Vec2(y, -x); }

		float Mag() const { return sqrtf(x*x + y*y); }
		float MagSquared() const { return x*x + y*y; }

		float x;
		float y;
	};

	// Undo record
	struct Undo
	{
		virtual ~Undo() { }
		virtual void Apply() = 0;
	};

	// Moveable object
	class Moveable : public Element
	{
	public:
		// Move by the given amount of time at the current velocity.
		// No accelerations, forces, or collisions are taken into
		// account here.  The time increment can be negative to back
		// up by an interval.
		virtual void Move(float dt) = 0;

		// Apply the nudge velocity.  This adds the nudge velocity
		// into the object's internal velocity, if applicable.  This
		// only applies to objects that can move relative to the 
		// playfield in response to a nudge, such as balls.  This
		// does nothing by default.
		virtual void Nudge(const Vec2 &vNudge) { }

		// Apply accelerations for a time step
		virtual void Accelerate(float dt) = 0;

		// create an undo record reflecting the current state
		virtual Undo *SaveUndo() = 0;
	};

	// Collidable object
	class Collidable
	{
	public:
		// Context structure.  Implementations can subclass this
		// to store custom context information during collision
		// testing, to pass back to the execute routine.
		struct Context
		{
			virtual ~Context() { }
		};

		// Test for a collision with the given object.  Returns the
		// amount of time the two objects have been in contact when
		// moving at their current velocities, so backing up the
		// simulation by this amount of time will place the objects
		// as they were at the moment of collision.  Zero means the
		// objects came into contact exactly at the current simulator
		// time.
		// 
		// If the objects aren't in contact, returns a negative 
		// value.  A negative value has no other meaning; in
		// particular, the routine isn't responsible for projecting
		// the time of a future collision.
		//
		// The point of separating the "test" and "execute" steps
		// is that it allows the simulator to search for the earliest
		// collision and execute it first.  When multiple collisions
		// occur on the same time step, it's possible that the
		// outcomes of collisions will be affected by earlier ones,
		// because the course of the ball changes after each.
		virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const = 0;

		// Execute a collision with the given object.  This is only
		// called after TestCollision() has returned an affirmative
		// result, and only after backing up the simulation to the
		// moment of collision encoded in the result, so this only
		// needs to carry out the velocity updates to the involved
		// objects.
		//
		// 'dtRev' is the reversal time from TestCollision().  Most
		// collision processors can ignore this, but it's sometimes
		// useful for complex objects (like flippers).
		virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) = 0;

		// Move for collision processing.  This handles the simulation
		// time adjustments before and after executing a collision.
		// For ordinary Moveable objects, this can simply invoke
		// Moveable::Move().
		virtual void CollisionMove(float dt, Context *ctx) = 0;
	};

	// Collidable list.  This can be used for composite components
	// made up of a collection of Collidable objects.  This provides 
	// the collidable methods to search for the earliest list element
	// with a collision.
	class CollidableList : public Collidable
	{
	public:
		// collidable objects
		std::list<Collidable*> collidable;

		// collision context
		struct CollCtx : Collidable::Context
		{
			CollCtx(Collidable *c, Collidable::Context *subCtx) : c(c), subCtx(subCtx) { }

			// winning object
			Collidable *c;

			// context from the winning object
			std::unique_ptr<Collidable::Context> subCtx;
		};

		// Collidable interface
		virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const override;
		virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) override;
		virtual void CollisionMove(float dt, Context *ctx) override;
	};

	// ball (moving object)
	class Ball : public Moveable, public Collidable
	{
	public:
		Ball(Table *table, const Point &c) : table(table), c(c) { }
		Ball(Table *table, const Point &c, const Vec2 &v) : table(table), c(c), v(v) { }
		Ball(Table *table, float x, float y) : table(table), c(x, y) { }
		Ball(Table *table, float x, float y, float vx, float vy) : table(table), c(x, y), v(vx, vy) { }
		Ball(Table *table, const Ball &ball) : table(table), c(ball.c), v(ball.v) { }

		virtual void Draw(DrawingCtx &ctx) const override;

		// containing table
		Table *table;

		// center location
		Point c;

		// speed
		Vec2 v;

		// Moveable
		virtual void Move(float dt) override { c = c + (v * dt); }
		virtual void Nudge(const Vec2 &vNudge) { v = v + vNudge; }
		virtual void Accelerate(float dt) override;

		// Collidable
		virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const override;
		virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) override;
		virtual void CollisionMove(float dt, Context*) override { Move(dt); }

		// Radius (mm).  Standard pinballs (used in virtually all commercial
		// pinball machines since about 1950) are 1-1/16" (27mm) in diameter.
		float r = 26.9875f/2.0f;

		// Mass (g).  Standard pinballs are 80.6g in mass.
		float m = 80.6f;

		// undo
		struct BallUndo : Undo
		{
			BallUndo(Ball *ball) : ball(ball), c(ball->c), v(ball->v) { }
			Ball *ball;
			Point c;
			Vec2 v;

			virtual void Apply()
			{
				ball->c = c;
				ball->v = v;
			}
		};
		virtual Undo *SaveUndo() override { return new BallUndo(this); }
	};

	// line segment
	class LineSeg : public Collidable
	{
	public:
		LineSeg(float e) : p1(0, 0), p2(0, 0), e(e) { }
		LineSeg(const Point &p1, const Point &p2, float e) : p1(p1), p2(p2), e(e) { }
		LineSeg(float x1, float y1, float x2, float y2, float e) : p1(x1, y1), p2(x2, y2), e(e) { }

		// endpoints
		Point p1;
		Point p2;

		// elasticity
		float e = 0.25f;

		// Collidable
		virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const override;
		virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) override;
		virtual void CollisionMove(float, Context*) override { }

		// calculate the distance from a point to the line
		float Dist(const Point &p) const {
			float dy = p2.y - p1.y, dx = p2.x - p1.x;
			return fabsf(dy*p.x - dx*p.y + p2.x*p.y - p1.x*p2.y)/sqrtf(dy*dy + dx*dx);
		}

		// project a point onto the line, returning the fraction of
		// the distance between the two points; if the intersection
		// is between the two points, the result is in 0..1
		float Project(const Point &p) const {
			float x21 = p2.x - p1.x, y21 = p2.y - p1.y;
			float xp1 = p.x - p1.x, yp1 = p.y - p1.y;
			return (xp1*x21 + yp1*y21)/(x21*x21 + y21*y21);
		}

		// interpolate the point for a projection
		Point Interpolate(float t) const {
			return Point(p1.x + (p2.x - p1.x)*t, p1.y + (p2.y - p1.y)*t);
		}
	};

	// Round.  This represents a stationary circular object,
	// such as a post.
	class Round : public Collidable
	{
	public:
		Round(float x, float y, float r, float e) : c(x, y), r(r), e(e) { }
		Round(const Point &pt, float r, float e) : c(pt), r(r), e(e) { }

		// Collidable
		virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const override;
		virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) override;
		virtual void CollisionMove(float, Context*) override { }

		// center point
		Point c;

		// radius
		float r;

		// elasticity
		float e;
	};

	// Bumper
	class Bumper : public Element, public Round
	{
	public:
		Bumper(float x, float y, float rApron, float rCap, float power) : Round(x, y, rApron, 0.0f), rCap(rCap), power(power) { }
		Bumper(const Point &pt, float rApron, float rCap, float power) : Round(pt, rApron, 0.0f), rCap(rCap), power(power) { }

		// cap diameter, for drawing
		float rCap;

		// jet power
		float power;

		// draw
		virtual void Draw(DrawingCtx &ctx) const override;

		// Collidable
		virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) override;
	};

	// Polygon.  Each point represents a vertex, which has a
	// rounding radius, and a line segment from each vertex
	// to the next vertex.
	//
	// We use polygons for two purposes:
	// 
	// 1. If the corner radius is non-zero, a polygon is a closed 
	// shape formed by connecting the vertices in clockwise order, 
	// with the last vertex connecting back to the first to close 
	// the shape.
	// 
	// 2. If the corner radius is zero, the polygon is open,
	// consisting only of the edges between adjacent vertices.
	// 
	// 
	class Polygon : public Element, public CollidableList
	{
	public:
		Polygon(float r, float e, const std::list<float> &points);
		Polygon(float r, float e, const std::list<Point> &points);
		Polygon(float r, float e) : r(e), e(e) { }

		void Init(const std::list<Point> &points);

		virtual void Draw(DrawingCtx &ctx) const override;

		// radius at each vertex
		float r;

		// elasticity
		float e;

		// vertices - used only when radius is non-zero
		std::list<Round> vertices;

		// edges
		std::list<LineSeg> edges;
	};

	// Stand-up target
	class Standup : public Element, public LineSeg
	{
	public:
		Standup(float x1, float y1, float x2, float y2, float e) : LineSeg(x1, y1, x2, y2, e) { }

		virtual void Draw(DrawingCtx &ctx) const override;
	};

	// One-way gate
	class Gate : public Element, public LineSeg
	{
	public:
		Gate(float x1, float y1, float x2, float y2, float e);

		virtual void Draw(DrawingCtx &ctx) const override;
		virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const override;
	};

	// Flipper.  
	class Flipper : public Moveable, public CollidableList
	{
	public:
		// leftButton = true if this flipper is controlled by the left button, false if controlled by the right button
		// x,y = center of rotation (flipper shaft location)
		// len = length between centers of rounded ends, in millimeters (70mm)
		// r1 = radius of the rotating end
		// r2 = radius of the rounded end at the moving end
		// restAngle = angle at rest position, in degrees in world coordinates (0 = due east/+X)
		// flipSpanAngle = flip distance, in degrees (angle between rest and flipped positions)
		// e = elasticity of the flipper rubber
		//
		// Typical measurements for actual pinball flippers:
		// 
		// Type                   len    r1     r2     rest   span   dist
		// Williams 1980s+        70.0   12.0   7.0    36     52     190
		// 
		// "rest" is the rest position relative to the horizontal.  In world
		// coordinates, this translates to 0-rest for a left flipper, and
		// 180+rest for a right flipper.
		// 
		// "dist" = distance between rotation centers.  The value listed is only
		// typical, since this isn't a fixed property of the flippers.
		//
		// flipSpanAngle is positive for a left flipper (counter-clockwise rotation),
		// negative for a right flipper (clockwise rotation)
		Flipper(Table *table, bool leftButton, float x, float y, float len, float r1, float r2, float restAngle, float flipSpanAngle, float e);

		virtual void Draw(DrawingCtx &ctx) const override;

		// Is the flipper energized?  
		bool energized = false;

		// containing table
		Table *table;

		// left/right button mapping
		bool leftButton;

		// elasticity
		float e;

		// center of rotation
		Point cr;

		// length - distance between centers
		float len;

		// radii of the ends; r1 is at the center of rotation, r2
		// is at the moving end
		float r1;
		float r2;

		// Rotation angles, resting (down) and flipped (up), in radians, where 0 is
		// due east (+X axis, right in the physics model and on-screen).  thetaDown
		// represents the resting position when the flipper isn't energized; thetaUp
		// is the maximum excursion position when fully flipped. 
		//
		// Left flipper: rotates CCW -> increasing theta on lift -> thetaUp > thetaDown
		// Right flipper: rotates CW -> decreasing theta on lift -> thetaUp < thetaDown
		float thetaDown;
		float thetaUp;

		// current angle of rotation, relative to thetaDown
		float theta = 0.0f;

		// Is the flipper at a stop with respect to a hit on the given edge?
		// This is the down position for a top hit, or the up position for a bottom hit.
		class FlipperEdge;
		bool IsAtStop(FlipperEdge *hitEdge) const {
			return (hitEdge == &topEdge && theta == thetaDown)
				|| (hitEdge == &bottomEdge && theta == thetaUp);
		}

		// current angular velocity
		float omega = 0.0f;

		// Estimated maximum angular velocity due to coil acceleration, 
		// reached at the top of the arc when the flipper is activated
		// from its resting position and completes the lift arc without
		// hitting anything that slows it down.  This is calculated from 
		// the lift alpha and flip angle.
		float omegaMax = 0.0f;

		// Top-of-arc stop time.  When the flipper reaches the top stop
		// on a flip, it abruptly stops.  A real flipper doesn't come to
		// an instant stop; it actually overshoots the top stop by a
		// fairly hefty margin - about 8 degrees in my test on a real
		// machine - before springing back to the top stop position.
		// The rapid deceleration also deforms the rubber ring.  These
		// details add some complexity to the physics that our simple
		// model doesn't fully capture, and in particular makes "live
		// catches" possible by creating a short time window where the 
		// flipper still has substantial upwards momentum, but not as
		// much as during the flip, and probably has reduced elasticity
		// due to the rubber deformation.  To better model this, we
		// track a short time window at the top stop, during which we
		// make some modifications to the collision parameters.
		//
		// This is set to the *remaining* time for special top-of-arc
		// handling.  When this is zero, it means that we're not in a
		// special processing time window.
		float topStopTime = 0.0f;

		// Top stop imaginary speed.  This simulates the overshoot
		// by retaining a portion of the final momentum for the 
		// purposes of collision calculation, with rapid falloff.
		float topStopOmega = 0.0f;

		// Unloaded acceleration for the lift coil, in radians/s^2.
		// For reference, a real 1990s pinball machine has an unloaded
		// flip time of about 25ms, which works out to 2900 rad/s^2
		// alpha.
		//
		// This parameter combines with 'I' (the moment of inertia)
		// to set the strength of the flipper in propelling balls.
		float alphaLift = 2000.0f;

		// Accelerate of stroke switch actuation position, as a fraction of
		// the stroke length.
		float eos = 0.96f;

		// Unloaded acceleration for the hold coil.  This is an estimate
		// based on the relative coil power.  It's hard to measure
		// directly, since the hold coil is only engaged at the top of
		// the flipper arc.  To measure this for real, you could hold
		// the EOS switch in the flipper UP position while timing a flip
		// from the rest position.
		//
		// The difference between the hold and lift power makes a raised
		// flipper "bouncier" than it would be from the elasticity of the
		// rubber alone, because the hold power is usually just enough to
		// hold the weight of the ball.  If the ball has any momentum
		// towards the flipper, that will tend to overpower the holding
		// force, pushing the flipper down until it crosses the EOS
		// switch, at which point the lift coil will re-engage and bring
		// the flipper back up to the top stop.  This makes the ball
		// bounce a bit as though the rubber were extra-elastic.  The
		// smaller the hold power fraction, the more bounce you'll see.
		float alphaHold = alphaLift/8.0f;

		// Flipper spring return acceleration, unloaded.  From physical
		// machine measurements.  Note that the return acceleration is
		// from a spring, so the acceleration isn't truly constant - it's
		// really a linear function of the current angle, since it comes
		// from the spring force, which is (displacement x k) where k is
		// the spring constant.  But constant acceleration is a very good
		// approximation, since the spring in a physical flipper assembly
		// is stretched even when the flipper is at rest, so the delta
		// displacement over the full range is small enough that the
		// spring force (and thus the acceleration) is practically
		// constant over the whole flipper range.
		float alphaSpring = 263.0f;

		// Moment of inertia of the flipper around its shaft axis, in
		// g*mm^2.  This is only nominally the 'I' of the flipper, but
		// what it really represents in the simulation is the effective
		// inertia of the whole moving portion of the flipper assembly,
		// including all of the under-playfield linkages.  This number
		// was chosen empirically to get the desired feel; I didn't try
		// to do any sort of rigorous analysis of the real mechanism.
		//
		// This value combines with alphaLift to set the strength of the
		// flipper.  Higher values will make the flipper stronger.
		float I = 3500.0f;

		// Friction, as a relative strength from 0 (none) to 1 (super glue)
		float friction = 0.075f;

		// Friction normal velocity curve points.  Friction drops to zero
		// when the normal velocity is V[min] or below, and is at full
		// strength (given by this->friction above) at V[full] or above,
		// with a ramp between the points.
		float frictionVMin = 10.0f;
		float frictionVFull = 50.0f;

		// Departure angle recording (for debugging/tuning)
		struct DepartureAngle
		{
			double dt = 0;      // time since start
			int phase = 0;      // capture phase: 0=start, 1=done
			Point ptActuate;    // ball position at actuation
			Point ptFinal;      // ball position at departure
		};
		std::list<DepartureAngle> departureAngle;
	
		// special handling for a flipper line segment; this is a line 
		// segment that can move at the flipper rotation rate
		class FlipperEdge : public LineSeg
		{
		public:
			FlipperEdge(Flipper *flipper) : LineSeg(flipper->e), flipper(flipper) { }
			virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const override;
			virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) override;
			virtual void CollisionMove(float dt, Context*) override { flipper->Move(dt); }
			Flipper *flipper;

			// Surface normal direction: 1 if the surface normal is
			// a left turn from the rotation center, -1 for a right turn.
			// 
			//  Flipper Side     Edge    Dir
			//    Left           Top      1
			//    Left           Bottom  -1
			//    Right          Top     -1
			//    Right          Bottom   1
			//
			float normalDir = 1.0f;
		};

		// special handling for the moving end of the flipper; this
		// is a subclass of Round that can move as the flipper rotates
		class FlipperEnd : public Round
		{
		public:
			FlipperEnd(Flipper *flipper, float x, float y, float r) : Round(x, y, r, flipper->e), flipper(flipper) { }
			virtual float TestCollision(const Ball &ball, std::unique_ptr<Context> &ctx, float dtMax) const override;
			virtual void ExecuteCollision(Ball &ball, Context *ctx, float dtRev) override;
			virtual void CollisionMove(float dt, Context*) override { flipper->Move(dt); }
			Flipper *flipper;
		};

		// clamp theta to the up/down range
		float ClampTheta(float theta) const {
			return thetaUp > thetaDown ?
				(theta < thetaDown ? thetaDown : theta > thetaUp ? thetaUp : theta) :
				(theta < thetaUp ? thetaUp : theta > thetaDown ? thetaDown : theta);
		}

		// Figure the contact distance between a ball and an edge at the 
		// given flipper theta.  This returns the distance from the center
		// of the ball to the edge along a normal to the edge.
		float CalcContactDistance(float theta, const FlipperEdge *edge, const Point &ballCenter, float ballRadius) const;

		// figure the edge line segment for a given flipper theta
		void CalcEdge(float theta, const FlipperEdge *edge, Point &p1, Point &p2) const;

		// current top and bottom edges, adjusted for rotation
		FlipperEdge topEdge;
		FlipperEdge bottomEdge;

		// Edge radius angle.  This is the angle of the normal to
		// the edge from the center of each end circle, relative
		// to the line between the two centers.
		float edgeRadiusAngle;

		// end circles
		Round round1;
		FlipperEnd round2;

		// set theta
		void SetTheta(float theta);

		// Moveable
		virtual void Move(float dt) override;
		virtual void Accelerate(float dt) override;

		// Undo
		struct FlipperUndo : Undo
		{
			FlipperUndo(Flipper *flipper) : flipper(flipper), theta(flipper->theta), omega(flipper->omega) { }
			Flipper *flipper;
			float theta;
			float omega;

			virtual void Apply() override
			{
				flipper->omega = omega;
				flipper->SetTheta(theta);
			}
		};
		virtual Undo *SaveUndo() override { return new FlipperUndo(this); }
	};

	// pin table model
	class Table
	{
	public:
		Table();

		// Draw.  The live object lists (flippers, balls) are supplied
		// so that the caller can make point-in-time copies for drawing
		// without keeping the model locked, for multi-threaded use.
		void Draw(DrawingCtx &ctx, const std::list<Ball> &balls, const std::list<Flipper> &flippers) const;

		// Set flipper button states
		void SetFlipperButtons(bool leftButtonPressed, bool rightButtonPressed);

		// Update nudge velocity.  This is for accelerometer devices that 
		// perform device-side velocity integration, such as Pinscape Pico.
		//
		// This is designed to improve on the original input model that VP
		// and most other simulators have traditionally used, where they
		// take accelerometer input in the form of direct acceleration
		// readings from the sensor.  Working in terms of accelerations is
		// problematic because the simulator doesn't share a sampling time
		// basis with the sensor, so it has the effect of resampling the
		// accelerometer input erratically.  Samples are therefore over-
		// or under-counted at random, greatly distorting the signal.  If
		// this were an audio signal it would sound like a ton of noise
		// had been added.  Moving the integration to the sensor solves
		// this problem, since the device has access to the sensor data
		// at its native sample rate.  The velocity data ISN'T affected by
		// the resampling problem at the simulator interface, because it's
		// part of the instantaneous state of the simulation.  It doesn't
		// matter that the simulator is sampling at a random rate relative
		// to the sensor, because all it cares about for velocity is the
		// instantaneous state, which can be sampled at any random time
		// and still be correct.
		void SetNudgeVelocity(float vx, float vy);

		// Update nudge acceleration.  This provides direct accelerometer
		// input using the raw acceleration readings from the sensor,
		// following the traditional VP input model.  This takes the input 
		// in 'g' (standard Earth gravity) units.
		void SetNudgeAccel(float ax, float ay);

		// Add a stationary object.  'r' is the radius of the
		// rounded corners.  The rest is a list of points that
		// define the vertices of the object
		void AddPolygon(float r, float e, std::list<float> points) { Add(&polygons.emplace_back(r, e, points)); }
		void AddPolygon(float r, float e, std::list<Point> points) { Add(&polygons.emplace_back(r, e, points)); }
		void AddPolygon(float r, float e) { Add(&polygons.emplace_back(r, e)); }

		// Add a stationary wall (a line segment)
		void AddWall(float e, float x1, float y1, float x2, float y2) { AddPolygon(0.0f, e, { { x1, y1}, { x2, y2 } }); }
		void AddWall(float e, const Point &p1, const Point &p2) { AddPolygon(0.0f, e, { p1, p2 }); }
		void AddWall(float e) { AddPolygon(0.0f, e); }

		// Add a ball
		void AddBall(float x, float y, float vx = 0, float vy = 0) { Add(&balls.emplace_back(this, x, y, vx, vy)); }

		// Add a flipper
		void AddFlipper(bool leftButton, float x, float y, float len, float r1, float r2, float angleAtRest, float angleSpan, float e) {
			Add(&flippers.emplace_back(this, leftButton, x, y, len, r1, r2, angleAtRest, angleSpan, e)); }

		// Get a snapshot of each of the stateful object lists
		std::list<Ball> GetBallSnapshot() { return balls; }
		std::list<Flipper> GetFlipperSnapshot() { return flippers; }

		// iterate balls/flippers
		void ForEachBall(std::function<void(Ball*)> func) { for (auto &b : balls) func(&b); }
		void ForEachFlipper(std::function<void(Flipper*)> func) { for (auto &f : flippers) func(&f); }

		// get the first ball/flipper
		Ball *FirstBall() { return balls.size() != 0 ? &balls.front() : nullptr; }
		Flipper *FirstFlipper() { return flippers.size() != 0 ? &flippers.front() : nullptr; }

		// Add an object
		void Add(Element *ele);
		
		// collision state
		struct Collision
		{
			float t = -1.0f;           // backup time to collision
			Ball *b = nullptr;         // ball
			Collidable *c = nullptr;   // collidable object
			std::unique_ptr<Collidable::Context> ctx;    // collidable object's private context

			void Set(float t, Ball *b, Collidable *c, std::unique_ptr<Collidable::Context> &ctx)
			{
				this->t = t;
				this->b = b;
				this->c = c;
				this->ctx.reset(ctx.release());
			}

			void Clear()
			{
				t = -1.0f;
				b = nullptr;
				c = nullptr;
				ctx.reset();
			}
		};

		// Evolve the simulation by an amount of time, in seconds
		void Evolve(float dt);

		// Undo to previous state, if capturing undo
		void UndoEvolve();

		// Debug mode - collects additional data for display
		void SetDebugMode(bool debugMode);
		bool IsDebugMode() const { return debugMode; }

		// set collision step mode
		void SetCollisionStepMode(bool enable) { evolveState.collisionStepMode = enable; }
		bool IsCollisionStepMode() const { return evolveState.collisionStepMode; }
		bool IsPreCollision() const { return evolveState.phase == EvolveState::Phase::CollisionExec; }

		// Undo capture mode
		void SetUndoCapture(bool enable);
		bool IsUndoCapture() const { return captureUndo; }

		// gravity on/off
		void EnableGravity(bool enable) { enableGravity = enable; }
		bool IsGravityEnabled() const { return enableGravity; }

	protected:
		// special modes
		bool debugMode = false;
		bool captureUndo = false;

		// gravity enabled
		bool enableGravity = true;;

		// time evolution state
		struct EvolveState
		{
			// time step
			float dt = 0.0005f;

			// maximum reversal time
			float dtReversalMax = dt;

			// Processing phase
			enum class Phase
			{
				Move,			   // start of step, before moving objects
				CollisionSearch,   // searching for next collision
				CollisionExec,     // finished processing collision
				Accelerate,        // end of step, after accelerations
			};
			Phase phase = Phase::Move;

			// Number of collisions remaining on this time step.  The time
			// step stops without considering any more collisions when this
			// limit is reached.
			int remainingCollisions = 100;

			// Collision step mode.  When this is true, Evolve() returns upon
			// finding or completing the next collision, without completing the
			// time step.
			bool collisionStepMode = false;

			// Pre-collision state.  When we're in collision-step mode, this
			// captures the state just prior to the collision
			Collision collision;
		};
		EvolveState evolveState;

		// Nudge direct accelerometer input state
		struct NudgeAccelState
		{
			// Current acceleration
			Vec2 a;

			// Cumulative nudge velocity
			Vec2 v;

			// Update for a time step.  Returns the change in velocity
			// over the time step.
			Vec2 Update(float dt);

			// Velocity half-life - time for accumulated velocity to
			// decay by 1/2.  In seconds.
			float halfLife = 1.0f;

			// Decay factor to apply per time step
			float dt = 0.0005f;
			float decayFactor = pow(0.5f, dt / 1.0f);
		};
		NudgeAccelState nudgeAccelState;

		// Nudge velocity input state
		struct NudgeVelocityState
		{
			// live device velocity
			Vec2 vDevice;

			// current velocity passed through to the model
			Vec2 vModel;

			// Update for a time step.  Transfers the live device velocity
			// into the model, and returns the difference between the new
			// live state and the old state.
			Vec2 Update();
		};
		NudgeVelocityState nudgeVelocityState;

		// Undo group.  This is a grouped collection of events that can
		// be undone as a set.
		struct UndoGroup
		{
			UndoGroup(EvolveState &state) :
				dtReversalMax(state.dtReversalMax),
				remainingCollisions(state.remainingCollisions),
				phase(state.phase)
			{ }

			// moved objects
			std::list<std::unique_ptr<Undo>> undo;

			// state restoration
			float dtReversalMax;
			int remainingCollisions;
			EvolveState::Phase phase;

			void Apply(EvolveState &state)
			{
				state.dtReversalMax = dtReversalMax;
				state.remainingCollisions = remainingCollisions;
				state.phase = phase;
				for (auto &u : undo)
					u->Apply();
			}
		};

		// undo group list
		std::list<UndoGroup> undo;

		// all table elements
		std::list<Element*> elements;

		// balls in play
		std::list<Ball> balls;

		// walls
		std::list<Polygon> polygons;

		// flippers
		std::list<Flipper> flippers;

		// Moveable objects
		std::list<Moveable*> moveable;

		// Collidable objects
		CollidableList collidable;
	};
}
