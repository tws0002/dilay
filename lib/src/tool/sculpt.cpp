#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QMouseEvent>
#include <QWheelEvent>
#include "action/sculpt.hpp"
#include "action/unit.hpp"
#include "cache.hpp"
#include "camera.hpp"
#include "color.hpp"
#include "config.hpp"
#include "history.hpp"
#include "primitive/ray.hpp"
#include "scene.hpp"
#include "sculpt-brush.hpp"
#include "state.hpp"
#include "tool/sculpt.hpp"
#include "tool/util/movement.hpp"
#include "view/cursor.hpp"
#include "view/properties.hpp"
#include "view/tool-tip.hpp"
#include "view/util.hpp"
#include "winged/face-intersection.hpp"
#include "winged/mesh.hpp"

struct ToolSculpt::Impl {
  ToolSculpt*                  self;
  SculptBrush                  brush;
  ViewCursor                   cursor;
  CacheProxy                   commonCache;
  std::unique_ptr <ActionUnit> actions;
  QDoubleSpinBox&              radiusEdit;

  Impl (ToolSculpt* s) 
    : self        (s) 
    , commonCache (this->self->cache ("sculpt"))
    , actions     (new ActionUnit) 
    , radiusEdit  (ViewUtil::spinBox (0.01f, 1.0f, 1000.0f, 10.0f))
  {}

  bool runAllowUndoRedo () const {
    return true;
  }

  ToolResponse runInitialize () {
    this->setupBrush      ();
    this->setupCursor     ();
    this->setupProperties ();
    this->setupToolTip    ();

    return ToolResponse::Redraw;
  }

  void setupBrush () {
    this->brush.detailFactor (this->self->config ().get <float> ("editor/tool/sculpt/detail-factor"));

    this->brush.radius          (this->commonCache.get <float> ("radius"           , 20.0f));
    this->brush.stepWidthFactor (this->commonCache.get <float> ("step-width-factor", 0.3f));
    this->brush.subdivide       (this->commonCache.get <bool>  ("subdivide"        , true));

    this->self->runSetupBrush (this->brush);
  }

  void setupCursor () {
    assert (this->brush.radius () > 0.0f);

    WingedFaceIntersection intersection;
    if (this->self->intersectsScene (this->self->cursorPosition (), intersection)) {
      this->cursor.enable   ();
      this->cursor.position (intersection.position ());
      this->cursor.normal   (intersection.normal   ());
    }
    else {
      this->cursor.disable ();
    }
    this->cursor.radius (this->brush.radius ());
    this->cursor.color  (this->commonCache.get <Color> ("cursor-color", Color::Red ()));

    this->self->runSetupCursor (this->cursor);
  }

  void setupProperties () {
    ViewPropertiesPart& properties = this->self->properties ().body ();

    this->radiusEdit.setValue (this->brush.radius ());
    ViewUtil::connect (this->radiusEdit, [this] (float r) {
      this->brush.radius (r);
      this->cursor.radius (r);
      this->commonCache.set ("radius", r);
    });
    properties.add (QObject::tr ("Radius"), this->radiusEdit);

    QDoubleSpinBox& stepEdit = ViewUtil::spinBox ( 0.01f, this->brush.stepWidthFactor ()
                                                 , 1000.0f, 0.1f );
    ViewUtil::connect (stepEdit, [this] (float s) {
      this->brush.stepWidthFactor (s);
      this->commonCache.set ("step-width-factor", s);
    });
    properties.add (QObject::tr ("Step width"), stepEdit);

    QCheckBox& subdivEdit = ViewUtil::checkBox ( QObject::tr ("Subdivide")
                                               , this->brush.subdivide () );
    QObject::connect (&subdivEdit, &QCheckBox::stateChanged, [this] (int s) {
      this->brush.subdivide (bool (s));
      this->commonCache.set ("subdivide", bool (s));
    });
    properties.add (subdivEdit);

    properties.add (ViewUtil::horizontalLine ());

    this->self->runSetupProperties (properties);
  }

  void setupToolTip () {
    ViewToolTip toolTip;

    this->self->runSetupToolTip (toolTip);
    toolTip.add ( ViewToolTip::MouseEvent::Wheel, ViewToolTip::Modifier::Shift
                , QObject::tr ("Change radius") );

    this->self->showToolTip (toolTip);
  }

  void runRender () const {
    this->cursor.render (this->self->state ().camera ());
  }

  ToolResponse runMouseReleaseEvent (const QMouseEvent& e) {
    if (e.button () == Qt::LeftButton) {
      this->brush.resetPosition ();
      this->addActionsToHistory ();
    }
    this->cursor.enable ();
    return ToolResponse::None;
  }

  void addActionsToHistory () {
    assert (this->actions);
    if (this->actions->isEmpty () == false) {
      this->self->state ().history ().addUnit (std::move (*this->actions));
      this->actions.reset (new ActionUnit ());
    }
  }

  ToolResponse runWheelEvent (const QWheelEvent& e) {
    if (e.orientation () == Qt::Vertical && e.modifiers () == Qt::ShiftModifier) {
      if (e.delta () > 0) {
        this->radiusEdit.stepUp ();
      }
      else if (e.delta () < 0) {
        this->radiusEdit.stepDown ();
      }
      ViewUtil::deselect (this->radiusEdit);
    }
    return ToolResponse::Redraw;
  }

  void runClose () {
    this->addActionsToHistory ();
  }

  void sculpt () {
    this->actions->add <ActionSculpt> ( this->self->state ().scene ()
                                      , this->brush.meshRef () ).run (this->brush);
  }

  void updateCursorByIntersection (const QMouseEvent& e) {
    WingedFaceIntersection intersection;

    if (this->self->intersectsScene (e, intersection)) {
      this->cursor.enable   ();
      this->cursor.position (intersection.position ());
      this->cursor.normal   (intersection.normal   ());
    }
    else {
      this->cursor.disable ();
    }
  }

  bool updateBrushAndCursorByIntersection (const QMouseEvent& e) {
    WingedFaceIntersection intersection;

    if (this->self->intersectsScene (e, intersection)) {
      this->cursor.enable   ();
      this->cursor.position (intersection.position ());
      this->cursor.normal   (intersection.normal   ());

      if (e.button () == Qt::LeftButton || e.buttons () == Qt::LeftButton) {
        this->brush.mesh (&intersection.mesh ());
        this->brush.face (&intersection.face ());

        return this->brush.updatePosition (intersection.position ());
      }
      else {
        return false;
      }
    }
    else {
      this->cursor.disable ();
      return false;
    }
  }

  void carvelikeStroke (const QMouseEvent& e, bool invertable) {
    if (this->updateBrushAndCursorByIntersection (e)) {
      if (invertable && e.modifiers () == Qt::ShiftModifier) {
        this->brush.toggleInvert ();
        this->sculpt ();
        this->brush.toggleInvert ();
      }
      else {
        this->sculpt ();
      }
    }
  }

  void initializeDraglikeStroke (const QMouseEvent& e, ToolUtilMovement& movement) {
    if (e.button () == Qt::LeftButton) {
      WingedFaceIntersection intersection;
      if (this->self->intersectsScene (e, intersection)) {
        this->brush.mesh        (&intersection.mesh     ());
        this->brush.face        (&intersection.face     ());
        this->brush.setPosition ( intersection.position ());
        
        this->cursor.disable ();

        movement.resetPosition (intersection.position ());
        movement.constraint    (MovementConstraint::CameraPlane);
      }
      else {
        this->cursor.enable ();
        this->brush.resetPosition ();
      }
    }
    else {
      this->cursor.enable ();
      this->brush.resetPosition ();
    }
  }

  void draglikeStroke (const QMouseEvent& e, ToolUtilMovement& movement) {
    if (e.buttons () == Qt::NoButton) {
      this->updateCursorByIntersection (e);
    }
    else if (e.buttons () == Qt::LeftButton && this->brush.hasPosition ()) {
      const glm::vec3 oldBrushPos = this->brush.position ();

      if ( movement.move (ViewUtil::toIVec2 (e))
        && this->brush.updatePosition (movement.position ()) )
      {
        this->brush.direction       (this->brush.position () - oldBrushPos);
        this->brush.intensityFactor (1.0f / this->brush.radius ());
        this->sculpt ();
      }
    }
  }
};

DELEGATE_BIG2_BASE (ToolSculpt, (State& s, const char* k), (this), Tool, (s, k))
GETTER         (SculptBrush&, ToolSculpt, brush)
GETTER         (ViewCursor& , ToolSculpt, cursor)
DELEGATE       (void        , ToolSculpt, sculpt)
DELEGATE1      (void        , ToolSculpt, updateCursorByIntersection, const QMouseEvent&)
DELEGATE1      (bool        , ToolSculpt, updateBrushAndCursorByIntersection, const QMouseEvent&)
DELEGATE2      (void        , ToolSculpt, carvelikeStroke, const QMouseEvent&, bool)
DELEGATE2      (void        , ToolSculpt, initializeDraglikeStroke, const QMouseEvent&, ToolUtilMovement&)
DELEGATE2      (void        , ToolSculpt, draglikeStroke, const QMouseEvent&, ToolUtilMovement&)
DELEGATE_CONST (bool        , ToolSculpt, runAllowUndoRedo)
DELEGATE       (ToolResponse, ToolSculpt, runInitialize)
DELEGATE_CONST (void        , ToolSculpt, runRender)
DELEGATE1      (ToolResponse, ToolSculpt, runMouseReleaseEvent, const QMouseEvent&)
DELEGATE1      (ToolResponse, ToolSculpt, runWheelEvent, const QWheelEvent&)
DELEGATE       (void        , ToolSculpt, runClose)
