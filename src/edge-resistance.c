/* Edge resistance for move/resize operations */

/* 
 * Copyright (C) 2005 Elijah Newren
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "edge-resistance.h"
#include "boxes.h"
#include "display.h"
#include "workspace.h"

/* A simple macro for whether a given window's edges are potentially
 * relevant for resistance/snapping during a move/resize operation
 */
#define WINDOW_EDGES_RELEVANT(window, display) \
  meta_window_should_be_showing (window) &&    \
  window->screen == display->grab_screen &&    \
  window         != display->grab_window &&    \
  window->type   != META_WINDOW_DESKTOP &&     \
  window->type   != META_WINDOW_MENU    &&     \
  window->type   != META_WINDOW_SPLASHSCREEN

struct ResistanceDataForAnEdge
{
  gboolean     timeout_setup;
  guint        timeout_id;
  int          timeout_edge_pos;
  gboolean     timeout_over;
  GSourceFunc  timeout_func;
  MetaWindow  *window;
  int          keyboard_buildup;
  gboolean     allow_past_screen_edge;
};
typedef struct ResistanceDataForAnEdge ResistanceDataForAnEdge;

struct MetaEdgeResistanceData
{
  GArray *left_edges;
  GArray *right_edges;
  GArray *top_edges;
  GArray *bottom_edges;

  ResistanceDataForAnEdge left_data;
  ResistanceDataForAnEdge right_data;
  ResistanceDataForAnEdge top_data;
  ResistanceDataForAnEdge bottom_data;
};

static int
find_index_of_edge_near_position (const GArray *edges,
                                  int           position,
                                  gboolean      want_interval_min,
                                  gboolean      horizontal)
{
  /* This is basically like a binary search, except that we're trying to
   * find a range instead of an exact value.  So, if we have in our array
   *   Value: 3  27 316 316 316 505 522 800 1213
   *   Index: 0   1   2   3   4   5   6   7    8
   * and we call this function with position=500 & want_interval_min=TRUE
   * then we should get 5 (because 505 is the first value bigger than 500).
   * If we call this function with position=805 and want_interval_min=FALSE
   * then we should get 7 (because 800 is the last value smaller than 800).
   * A couple more, to make things clear:
   *    position  want_interval_min  correct_answer
   *         316               TRUE               2
   *         316              FALSE               4
   *           2              FALSE              -1
   *        2000               TRUE               9
   */
  int low, high, mid;
  int compare;
  MetaEdge *edge;

  /* Initialize mid, edge, & compare in the off change that the array only
   * has one element.
   */
  mid  = 0;
  edge = g_array_index (edges, MetaEdge*, mid);
  compare = horizontal ? edge->rect.x : edge->rect.y;

  /* Begin the search... */
  low  = 0;
  high = edges->len - 1;
  while (low < high)
    {
      mid = low + (high - low)/2;
      edge = g_array_index (edges, MetaEdge*, mid);
      compare = horizontal ? edge->rect.x : edge->rect.y;

      if (compare == position)
        break;

      if (compare > position)
        high = mid - 1;
      else
        low = mid + 1;
    }

  /* mid should now be _really_ close to the index we want, so we start
   * linearly searching.  However, note that we don't know if mid is less
   * than or greater than what we need and it's possible that there are
   * several equal values equal to what we were searching for and we ended
   * up in the middle of them instead of at the end.  So we may need to
   * move mid multiple locations over.
   */
  if (want_interval_min)
    {
      while (compare >= position && mid > 0)
        {
          mid--;
          edge = g_array_index (edges, MetaEdge*, mid);
          compare = horizontal ? edge->rect.x : edge->rect.y;
        }
      while (compare < position && mid < (int)edges->len - 1)
        {
          mid++;
          edge = g_array_index (edges, MetaEdge*, mid);
          compare = horizontal ? edge->rect.x : edge->rect.y;
        }

      /* Special case for no values in array big enough */
      if (compare < position)
        return edges->len;

      /* Return the found value */
      return mid;
    }
  else
    {
      while (compare <= position && mid < (int)edges->len - 1)
        {
          mid++;
          edge = g_array_index (edges, MetaEdge*, mid);
          compare = horizontal ? edge->rect.x : edge->rect.y;
        }
      while (compare > position && mid > 0)
        {
          mid--;
          edge = g_array_index (edges, MetaEdge*, mid);
          compare = horizontal ? edge->rect.x : edge->rect.y;
        }

      /* Special case for no values in array small enough */
      if (compare > position)
        return -1;

      /* Return the found value */
      return mid;
    }
}

static gboolean
points_on_same_side (int ref, int pt1, int pt2)
{
  return (pt1 - ref) * (pt2 - ref) > 0;
}

static int
find_nearest_position (const GArray        *edges,
                       int                  position,
                       int                  old_position,
                       const MetaRectangle *new_rect,
                       gboolean             horizontal,
                       gboolean             only_forward)
{
  /* This is basically just a binary search except that we're looking
   * for the value closest to position, rather than finding that
   * actual value.  Also, we ignore any edges that aren't relevant
   * given the horizontal/vertical position of new_rect.
   */
  int low, high, mid;
  int compare;
  MetaEdge *edge;
  int best, best_dist, i;

  /* Initialize mid, edge, & compare in the off change that the array only
   * has one element.
   */
  mid  = 0;
  edge = g_array_index (edges, MetaEdge*, mid);
  compare = horizontal ? edge->rect.x : edge->rect.y;

  /* Begin the search... */
  low  = 0;
  high = edges->len - 1;
  while (low < high)
    {
      mid = low + (high - low)/2;
      edge = g_array_index (edges, MetaEdge*, mid);
      compare = horizontal ? edge->rect.x : edge->rect.y;

      if (compare == position)
        break;

      if (compare > position)
        high = mid - 1;
      else
        low = mid + 1;
    }

  /* mid should now be _really_ close to the index we want, so we
   * start searching nearby for something that overlaps and is closer
   * than the original position.
   */
  best = old_position;
  best_dist = INT_MAX;

  /* Start the search at mid */
  edge = g_array_index (edges, MetaEdge*, mid);
  compare = horizontal ? edge->rect.x : edge->rect.y;
  gboolean edges_align = horizontal ? 
    meta_rectangle_vert_overlap (&edge->rect, new_rect) :
    meta_rectangle_horiz_overlap (&edge->rect, new_rect);
  if (edges_align &&
      (!only_forward || !points_on_same_side (position, compare, old_position)))
    {
      int dist = ABS (compare - position);
      if (dist < best_dist)
        {
          best = compare;
          best_dist = dist;
        }
    }

  /* Now start searching higher than mid */
  for (i = mid + 1; i < (int)edges->len; i++)
    {
      edge = g_array_index (edges, MetaEdge*, i);
      compare = horizontal ? edge->rect.x : edge->rect.y;
  
      gboolean edges_align = horizontal ? 
        meta_rectangle_vert_overlap (&edge->rect, new_rect) :
        meta_rectangle_horiz_overlap (&edge->rect, new_rect);

      if (edges_align &&
          (!only_forward ||
           !points_on_same_side (position, compare, old_position)))
        {
          int dist = ABS (compare - position);
          if (dist < best_dist)
            {
              best = compare;
              best_dist = dist;
            }
          break;
        }
    }

  /* Now start searching lower than mid */
  for (i = mid-1; i >= 0; i--)
    {
      edge = g_array_index (edges, MetaEdge*, i);
      compare = horizontal ? edge->rect.x : edge->rect.y;
  
      gboolean edges_align = horizontal ? 
        meta_rectangle_vert_overlap (&edge->rect, new_rect) :
        meta_rectangle_horiz_overlap (&edge->rect, new_rect);

      if (edges_align &&
          (!only_forward ||
           !points_on_same_side (position, compare, old_position)))
        {
          int dist = ABS (compare - position);
          if (dist < best_dist)
            {
              best = compare;
              best_dist = dist;
            }
          break;
        }
    }

  /* Return the best one found */
  return best;
}

static gboolean
movement_towards_edge (MetaDirection side, int increment)
{
  switch (side)
    {
    case META_DIRECTION_LEFT:
    case META_DIRECTION_TOP:
      return increment < 0;
    case META_DIRECTION_RIGHT:
    case META_DIRECTION_BOTTOM:
      return increment > 0;
    }

  g_assert_not_reached ();
}

static gboolean
edge_resistance_timeout (gpointer data)
{
  ResistanceDataForAnEdge *resistance_data = data;

  resistance_data->timeout_over = TRUE;
  resistance_data->timeout_id = 0;
  (*resistance_data->timeout_func)(resistance_data->window);

  return FALSE;
}

static int
apply_edge_resistance (MetaWindow                *window,
                       int                        old_pos,
                       int                        new_pos,
                       const MetaRectangle       *new_rect,
                       GArray                    *edges,
                       ResistanceDataForAnEdge   *resistance_data,
                       GSourceFunc                timeout_func,
                       gboolean                   xdir,
                       gboolean                   keyboard_op)
{
  int i, begin, end;
  gboolean okay_to_clear_keyboard_buildup = FALSE;
  int      keyboard_buildup_edge = G_MAXINT;
  gboolean increasing = new_pos > old_pos;
  int      increment = increasing ? 1 : -1;

  const int PIXEL_DISTANCE_THRESHOLD_TOWARDS_WINDOW    = 16;
  const int PIXEL_DISTANCE_THRESHOLD_AWAYFROM_WINDOW   =  8;
  const int PIXEL_DISTANCE_THRESHOLD_TOWARDS_XINERAMA  = 32;
  const int PIXEL_DISTANCE_THRESHOLD_AWAYFROM_XINERAMA =  8;
  const int PIXEL_DISTANCE_THRESHOLD_TOWARDS_SCREEN    = 32;
  const int PIXEL_DISTANCE_THRESHOLD_AWAYFROM_SCREEN   =  8;
  const int TIMEOUT_RESISTANCE_LENGTH_MS_WINDOW   =   0;
  const int TIMEOUT_RESISTANCE_LENGTH_MS_XINERAMA = 100;
  const int TIMEOUT_RESISTANCE_LENGTH_MS_SCREEN   = 750;
  const int KEYBOARD_BUILDUP_THRESHOLD_TOWARDS_WINDOW    = 16;
  const int KEYBOARD_BUILDUP_THRESHOLD_AWAYFROM_WINDOW   = 16;
  const int KEYBOARD_BUILDUP_THRESHOLD_TOWARDS_XINERAMA  = 24;
  const int KEYBOARD_BUILDUP_THRESHOLD_AWAYFROM_XINERAMA = 16;
  const int KEYBOARD_BUILDUP_THRESHOLD_TOWARDS_SCREEN    = 32;
  const int KEYBOARD_BUILDUP_THRESHOLD_AWAYFROM_SCREEN   = 16;

  /* Quit if no movement was specified */
  if (old_pos == new_pos)
    return new_pos;

  /* Remove the old timeout if it's no longer relevant */
  if (resistance_data->timeout_setup &&
      ((resistance_data->timeout_edge_pos > old_pos &&
        resistance_data->timeout_edge_pos > new_pos)  ||
       (resistance_data->timeout_edge_pos < old_pos &&
        resistance_data->timeout_edge_pos < new_pos)))
    {
      resistance_data->timeout_setup = FALSE;
      if (resistance_data->timeout_id != 0)
        {
          g_source_remove (resistance_data->timeout_id);
          resistance_data->timeout_id = 0;
        }
    }

  /* Get the range of indices in the edge array that we move past/to. */
  begin = find_index_of_edge_near_position (edges, old_pos,  increasing, xdir);
  end   = find_index_of_edge_near_position (edges, new_pos, !increasing, xdir);

  /* Loop over all these edges we're moving past/to. */
  i = begin;
  while ((increasing  && i <= end) ||
         (!increasing && i >= end))
    {
      gboolean  edges_align;
      MetaEdge *edge = g_array_index (edges, MetaEdge*, i);
      int       compare = xdir ? edge->rect.x : edge->rect.y;

      /* Find out if this edge is relevant */
      edges_align = xdir ? 
        meta_rectangle_vert_overlap (&edge->rect, new_rect) :
        meta_rectangle_horiz_overlap (&edge->rect, new_rect);

      /* Nothing to do unless the edges align */
      if (!edges_align)
        {
          /* Go to the next edge in the range */
          i += increment;
          continue;
        }

      /* Rest is easier to read if we split on keyboard vs. mouse op */
      if (keyboard_op)
        {
          /* KEYBOARD ENERGY BUILDUP RESISTANCE: If the user has is moving
           * fast enough or has already built up enough "energy", then let
           * the user past the edge, otherwise stop at this edge.  If the
           * user was previously stopped at this edge, add movement amount
           * to the built up energy.
           */

          /* First, determine the amount of the resistance */
          int resistance = 0;
          switch (edge->edge_type)
            {
            case META_EDGE_WINDOW:
              if (movement_towards_edge (edge->side_type, increment))
                resistance = KEYBOARD_BUILDUP_THRESHOLD_TOWARDS_WINDOW;
              else
                resistance = KEYBOARD_BUILDUP_THRESHOLD_AWAYFROM_WINDOW;
              break;
            case META_EDGE_XINERAMA:
              if (movement_towards_edge (edge->side_type, increment))
                resistance = KEYBOARD_BUILDUP_THRESHOLD_TOWARDS_XINERAMA;
              else
                resistance = KEYBOARD_BUILDUP_THRESHOLD_AWAYFROM_XINERAMA;
              break;
            case META_EDGE_SCREEN:
              if (movement_towards_edge (edge->side_type, increment))
                resistance = KEYBOARD_BUILDUP_THRESHOLD_TOWARDS_SCREEN;
              else
                resistance = KEYBOARD_BUILDUP_THRESHOLD_AWAYFROM_SCREEN;
              break;
            }

          /* Clear any previous buildup if we've run into an edge at a
           * different location than what we were building up on before.
           * See below for more details where these get set.
           */
          if (okay_to_clear_keyboard_buildup &&
              compare != keyboard_buildup_edge)
            {
              okay_to_clear_keyboard_buildup = FALSE;
              resistance_data->keyboard_buildup = 0;
            }

          /* Determine the threshold */
          int threshold = resistance - resistance_data->keyboard_buildup;

          /* See if threshold hasn't been met yet or not */
          if (ABS (compare - new_pos) < threshold)
            {
              if (resistance_data->keyboard_buildup != 0)
                resistance_data->keyboard_buildup += ABS (new_pos - compare);
              else
                resistance_data->keyboard_buildup = 1; /* 0 causes stuckage */
              return compare;
            }
          else
            {
              /* It may be the case that there are two windows with edges
               * at the same location.  If so, the buildup ought to count
               * towards both edges.  So we just not that it's okay to
               * clear the buildup once we find an edge at a different
               * location.
               */
              okay_to_clear_keyboard_buildup = TRUE;
              keyboard_buildup_edge = compare;
            }
        }
      else /* mouse op */
        {
          /* INFINITE RESISTANCE for screen edges under certain cases; If
           * the edge is relevant and we're moving towards it and it's a
           * screen edge and infinite resistance has been requested for
           * this particular grab op then don't allow movement past it.
           */
          if (edge->edge_type == META_EDGE_SCREEN &&
              !resistance_data->allow_past_screen_edge &&
              movement_towards_edge (edge->side_type, increment))
            {
              return compare;
            }

          /* TIMEOUT RESISTANCE: If the edge is relevant and we're moving
           * towards it, then we may want to have some kind of time delay
           * before the user can move past this edge.
           */
          if (movement_towards_edge (edge->side_type, increment))
            {
              /* First, determine the length of time for the resistance */
              int timeout_length_ms = 0;
              switch (edge->edge_type)
                {
                case META_EDGE_WINDOW:
                  timeout_length_ms = TIMEOUT_RESISTANCE_LENGTH_MS_WINDOW;
                  break;
                case META_EDGE_XINERAMA:
                  if (window->require_on_single_xinerama)
                    timeout_length_ms = TIMEOUT_RESISTANCE_LENGTH_MS_XINERAMA;
                  break;
                case META_EDGE_SCREEN:
                  if (window->require_fully_onscreen)
                    timeout_length_ms = TIMEOUT_RESISTANCE_LENGTH_MS_SCREEN;
                  break;
                }

              if (!resistance_data->timeout_setup &&
                  timeout_length_ms != 0)
                {
                  resistance_data->timeout_id = 
                    g_timeout_add (timeout_length_ms,
                                   edge_resistance_timeout,
                                   resistance_data);
                  resistance_data->timeout_setup = TRUE;
                  resistance_data->timeout_edge_pos = compare;
                  resistance_data->timeout_over = FALSE;
                  resistance_data->timeout_func = timeout_func;
                  resistance_data->window = window;
                }
              if (!resistance_data->timeout_over &&
                  timeout_length_ms != 0)
                return compare;
            }

          /* PIXEL DISTANCE MOUSE RESISTANCE: If the edge matters and the
           * user hasn't moved at least threshold pixels past this edge,
           * stop movement at this edge.  (Note that this is different from
           * keyboard resistance precisely because keyboard move ops are
           * relative to previous positions, whereas mouse move ops are
           * relative to differences in mouse position and mouse position
           * is an absolute quantity rather than a relative quantity)
           */

          /* First, determine the threshold */
          int threshold = 0;
          switch (edge->edge_type)
            {
            case META_EDGE_WINDOW:
              if (movement_towards_edge (edge->side_type, increment))
                threshold = PIXEL_DISTANCE_THRESHOLD_TOWARDS_WINDOW;
              else
                threshold = PIXEL_DISTANCE_THRESHOLD_AWAYFROM_WINDOW;
              break;
            case META_EDGE_XINERAMA:
              if (movement_towards_edge (edge->side_type, increment))
                threshold = PIXEL_DISTANCE_THRESHOLD_TOWARDS_XINERAMA;
              else
                threshold = PIXEL_DISTANCE_THRESHOLD_AWAYFROM_XINERAMA;
              break;
            case META_EDGE_SCREEN:
              if (movement_towards_edge (edge->side_type, increment))
                threshold = PIXEL_DISTANCE_THRESHOLD_TOWARDS_SCREEN;
              else
                threshold = PIXEL_DISTANCE_THRESHOLD_AWAYFROM_SCREEN;
              break;
            }

          if (ABS (compare - new_pos) < threshold)
            return compare;
        }

      /* Go to the next edge in the range */
      i += increment;
    }

  /* If we didn't run into any new edges in keyboard buildup but had moved
   * far enough to get past the last one, clear the buildup
   */
  if (okay_to_clear_keyboard_buildup && new_pos != keyboard_buildup_edge)
    resistance_data->keyboard_buildup = 0;

  return new_pos;
}

static int
apply_edge_snapping (int                  old_pos,
                     int                  new_pos,
                     const MetaRectangle *new_rect,
                     GArray              *edges1,
                     GArray              *edges2,
                     gboolean             xdir,
                     gboolean             keyboard_op)
{
  int pos1, pos2;
  int best;

  if (old_pos == new_pos)
    return new_pos;

  /* We look at two sets of edges (e.g. left and right) individually
   * finding the nearest position among each set of edges and then later
   * finding the better of these two bests.
   */
  pos1 = find_nearest_position (edges1,
                                new_pos,
                                old_pos,
                                new_rect,
                                xdir,
                                keyboard_op);
  pos2 = find_nearest_position (edges2,
                                new_pos,
                                old_pos,
                                new_rect,
                                xdir,
                                keyboard_op);

  /* For keyboard snapping, ignore either pos1 or pos2 if they aren't in the
   * right direction.
   */
  if (keyboard_op)
    {
      if (!points_on_same_side (old_pos, pos1, new_pos))
        return pos2;
      if (!points_on_same_side (old_pos, pos2, new_pos))
        return pos1;
    }

  /* Find the better of pos1 and pos2 and return it */
  if (ABS (pos1 - new_pos) < ABS (pos2 - new_pos))
    best = pos1;
  else
    best = pos2;

  /* If mouse snap-moving, the user could easily accidentally move just a
   * couple pixels in a direction they didn't mean to move; so ignore snap
   * movement in those cases unless it's only a small number of pixels
   * anyway.
   */
  if (!keyboard_op &&
      ABS (best - old_pos) >= 8 &&
      ABS (new_pos - old_pos) < 8)
    return old_pos;
  else
    /* Otherwise, return the best of the snapping positions found */
    return best;
}

/* This function takes the position (including any frame) of the window and
 * a proposed new position (ignoring edge resistance/snapping), and then
 * applies edge resistance to EACH edge (separately) updating new_outer.
 * It returns true if new_outer is modified, false otherwise.
 *
 * display->grab_edge_resistance_data MUST already be setup or calling this
 * function will cause a crash.
 */
static gboolean 
apply_edge_resistance_to_each_side (MetaDisplay         *display,
                                    MetaWindow          *window,
                                    const MetaRectangle *old_outer,
                                    MetaRectangle       *new_outer,
                                    GSourceFunc          timeout_func,
                                    gboolean             auto_snap,
                                    gboolean             keyboard_op)
{
  MetaEdgeResistanceData *edge_data;
  MetaRectangle           modified_rect;
  gboolean                modified;
  int new_left, new_right, new_top, new_bottom;

  g_assert (display->grab_edge_resistance_data != NULL);
  edge_data = display->grab_edge_resistance_data;

  if (auto_snap)
    {
      /* Do the auto snapping instead of normal edge resistance; in all
       * cases, we allow snapping to opposite kinds of edges (e.g. left
       * sides of windows to both left and right edges.
       */

      new_left   = apply_edge_snapping (BOX_LEFT (*old_outer),
                                        BOX_LEFT (*new_outer),
                                        new_outer,
                                        edge_data->left_edges,
                                        edge_data->right_edges,
                                        TRUE,
                                        keyboard_op);

      new_right  = apply_edge_snapping (BOX_RIGHT (*old_outer),
                                        BOX_RIGHT (*new_outer),
                                        new_outer,
                                        edge_data->left_edges,
                                        edge_data->right_edges,
                                        TRUE,
                                        keyboard_op);

      new_top    = apply_edge_snapping (BOX_TOP (*old_outer),
                                        BOX_TOP (*new_outer),
                                        new_outer,
                                        edge_data->top_edges,
                                        edge_data->bottom_edges,
                                        FALSE,
                                        keyboard_op);

      new_bottom = apply_edge_snapping (BOX_BOTTOM (*old_outer),
                                        BOX_BOTTOM (*new_outer),
                                        new_outer,
                                        edge_data->top_edges,
                                        edge_data->bottom_edges,
                                        FALSE,
                                        keyboard_op);
    }
  else
    {
      /* Now, apply the normal edge resistance */
      new_left   = apply_edge_resistance (window,
                                          BOX_LEFT (*old_outer),
                                          BOX_LEFT (*new_outer),
                                          new_outer,
                                          edge_data->left_edges,
                                          &edge_data->left_data,
                                          timeout_func,
                                          TRUE,
                                          keyboard_op);
      new_right  = apply_edge_resistance (window,
                                          BOX_RIGHT (*old_outer),
                                          BOX_RIGHT (*new_outer),
                                          new_outer,
                                          edge_data->right_edges,
                                          &edge_data->right_data,
                                          timeout_func,
                                          TRUE,
                                          keyboard_op);
      new_top    = apply_edge_resistance (window,
                                          BOX_TOP (*old_outer),
                                          BOX_TOP (*new_outer),
                                          new_outer,
                                          edge_data->top_edges,
                                          &edge_data->top_data,
                                          timeout_func,
                                          FALSE,
                                          keyboard_op);
      new_bottom = apply_edge_resistance (window,
                                          BOX_BOTTOM (*old_outer),
                                          BOX_BOTTOM (*new_outer),
                                          new_outer,
                                          edge_data->bottom_edges,
                                          &edge_data->bottom_data,
                                          timeout_func,
                                          FALSE,
                                          keyboard_op);
    }

  /* Determine whether anything changed, and save the changes */
  modified_rect = meta_rect (new_left, 
                             new_top,
                             new_right - new_left,
                             new_bottom - new_top);
  modified = !meta_rectangle_equal (new_outer, &modified_rect);
  *new_outer = modified_rect;
  return modified;
}

void
meta_display_cleanup_edges (MetaDisplay *display)
{
  MetaEdgeResistanceData *edge_data = display->grab_edge_resistance_data;
  g_assert (edge_data != NULL);
  guint i,j;

  /* We first need to clean out any window edges */
  for (i = 0; i < 4; i++)
    {
      GArray *tmp = NULL;
      switch (i)
        {
        case 0:
          tmp = edge_data->left_edges;
          break;
        case 1:
          tmp = edge_data->right_edges;
          break;
        case 2:
          tmp = edge_data->top_edges;
          break;
        case 3:
          tmp = edge_data->bottom_edges;
          break;
        default:
          g_assert_not_reached ();
        }

      for (j = 0; j < tmp->len; j++)
        {
          MetaEdge *edge = g_array_index (tmp, MetaEdge*, j);
          if (edge->edge_type == META_EDGE_WINDOW)
            g_free (edge);
        }
    }

  /* Now free the arrays and data */
  g_array_free (edge_data->left_edges, TRUE);
  g_array_free (edge_data->right_edges, TRUE);
  g_array_free (edge_data->top_edges, TRUE);
  g_array_free (edge_data->bottom_edges, TRUE);

  /* Cleanup the timeouts */
  if (edge_data->left_data.timeout_setup   &&
      edge_data->left_data.timeout_id   != 0)
    g_source_remove (edge_data->left_data.timeout_id);
  if (edge_data->right_data.timeout_setup  &&
      edge_data->right_data.timeout_id  != 0)
    g_source_remove (edge_data->right_data.timeout_id);
  if (edge_data->top_data.timeout_setup    &&
      edge_data->top_data.timeout_id    != 0)
    g_source_remove (edge_data->top_data.timeout_id);
  if (edge_data->bottom_data.timeout_setup &&
      edge_data->bottom_data.timeout_id != 0)
    g_source_remove (edge_data->bottom_data.timeout_id);

  g_free (display->grab_edge_resistance_data);
  display->grab_edge_resistance_data = NULL;
}

static int
stupid_sort_requiring_extra_pointer_dereference (gconstpointer a, 
                                                 gconstpointer b)
{
  const MetaEdge * const *a_edge = a;
  const MetaEdge * const *b_edge = b;
  return meta_rectangle_edge_cmp (*a_edge, *b_edge);
}

static void
cache_edges (MetaDisplay *display,
             GList *window_edges,
             GList *xinerama_edges,
             GList *screen_edges)
{
  MetaEdgeResistanceData *edge_data;
  GList *tmp;
  int num_left, num_right, num_top, num_bottom;
  int i;

  /*
   * 1st: Get the total number of each kind of edge
   */
  num_left = num_right = num_top = num_bottom = 0;
  for (i = 0; i < 3; i++)
    {
      tmp = NULL;
      switch (i)
        {
        case 0:
          tmp = window_edges;
          break;
        case 1:
          tmp = xinerama_edges;
          break;
        case 2:
          tmp = screen_edges;
          break;
        default:
          g_assert_not_reached ();
        }

      while (tmp)
        {
          MetaEdge *edge = tmp->data;
          switch (edge->side_type)
            {
            case META_DIRECTION_LEFT:
              num_left++;
              break;
            case META_DIRECTION_RIGHT:
              num_right++;
              break;
            case META_DIRECTION_TOP:
              num_top++;
              break;
            case META_DIRECTION_BOTTOM:
              num_bottom++;
              break;
            default:
              g_assert_not_reached ();
            }
          tmp = tmp->next;
        }
    }

  /*
   * 2nd: Allocate the edges
   */
  g_assert (display->grab_edge_resistance_data == NULL);
  display->grab_edge_resistance_data = g_new (MetaEdgeResistanceData, 1);
  edge_data = display->grab_edge_resistance_data;
  edge_data->left_edges   = g_array_sized_new (FALSE,
                                               FALSE,
                                               sizeof(MetaEdge*),
                                               num_left);
  edge_data->right_edges  = g_array_sized_new (FALSE,
                                               FALSE,
                                               sizeof(MetaEdge*),
                                               num_right);
  edge_data->top_edges    = g_array_sized_new (FALSE,
                                               FALSE,
                                               sizeof(MetaEdge*),
                                               num_top);
  edge_data->bottom_edges = g_array_sized_new (FALSE,
                                               FALSE,
                                               sizeof(MetaEdge*),
                                               num_bottom);

  /*
   * 3rd: Add the edges to the arrays
   */
  num_left = num_right = num_top = num_bottom = 0;
  for (i = 0; i < 3; i++)
    {
      tmp = NULL;
      switch (i)
        {
        case 0:
          tmp = window_edges;
          break;
        case 1:
          tmp = xinerama_edges;
          break;
        case 2:
          tmp = screen_edges;
          break;
        default:
          g_assert_not_reached ();
        }

      while (tmp)
        {
          MetaEdge *edge = tmp->data;
          switch (edge->side_type)
            {
            case META_DIRECTION_LEFT:
              g_array_append_val (edge_data->left_edges, edge);
              break;
            case META_DIRECTION_RIGHT:
              g_array_append_val (edge_data->right_edges, edge);
              break;
            case META_DIRECTION_TOP:
              g_array_append_val (edge_data->top_edges, edge);
              break;
            case META_DIRECTION_BOTTOM:
              g_array_append_val (edge_data->bottom_edges, edge);
              break;
            default:
              g_assert_not_reached ();
            }
          tmp = tmp->next;
        }
    }

  /*
   * 4th: Sort the arrays (FIXME: This is kinda dumb since the arrays were
   * individually sorted earlier and we could have done this faster and
   * avoided this sort by sticking them into the array with some simple
   * merging of the lists).
   */
  g_array_sort (display->grab_edge_resistance_data->left_edges, 
                stupid_sort_requiring_extra_pointer_dereference);
  g_array_sort (display->grab_edge_resistance_data->right_edges, 
                stupid_sort_requiring_extra_pointer_dereference);
  g_array_sort (display->grab_edge_resistance_data->top_edges, 
                stupid_sort_requiring_extra_pointer_dereference);
  g_array_sort (display->grab_edge_resistance_data->bottom_edges, 
                stupid_sort_requiring_extra_pointer_dereference);
}

static void
initialize_grab_edge_resistance_data (MetaDisplay *display)
{
  MetaEdgeResistanceData *edge_data = display->grab_edge_resistance_data;

  edge_data->left_data.timeout_setup   = FALSE;
  edge_data->right_data.timeout_setup  = FALSE;
  edge_data->top_data.timeout_setup    = FALSE;
  edge_data->bottom_data.timeout_setup = FALSE;

  edge_data->left_data.keyboard_buildup   = 0;
  edge_data->right_data.keyboard_buildup  = 0;
  edge_data->top_data.keyboard_buildup    = 0;
  edge_data->bottom_data.keyboard_buildup = 0;

  edge_data->left_data.allow_past_screen_edge   = TRUE;
  edge_data->right_data.allow_past_screen_edge  = TRUE;
  edge_data->bottom_data.allow_past_screen_edge = TRUE;
  edge_data->top_data.allow_past_screen_edge    = 
    display->grab_anchor_root_y >= display->grab_initial_window_pos.y;
}

void
meta_display_compute_resistance_and_snapping_edges (MetaDisplay *display)
{
  GList *stacked_windows;
  GList *cur_window_iter;
  GList *edges;
  /* Lists of window positions (rects) and their relative stacking positions */
  int stack_position;
  GSList *obscuring_windows, *window_stacking;
  /* The portions of the above lists that still remain at the stacking position
   * in the layer that we are working on
   */
  GSList *rem_windows, *rem_win_stacking;

  /*
   * 1st: Get the list of relevant windows, from bottom to top
   */
  stacked_windows = 
    meta_stack_list_windows (display->grab_screen->stack,
                             display->grab_screen->active_workspace);

  /*
   * 2nd: we need to separate that stacked list into a list of windows that
   * can obscure other edges.  To make sure we only have windows obscuring
   * those below it instead of going both ways, we also need to keep a
   * counter list.  Messy, I know.
   */
  obscuring_windows = window_stacking = NULL;
  cur_window_iter = stacked_windows;
  stack_position = 0;
  while (cur_window_iter != NULL)
    {
      MetaWindow *cur_window = cur_window_iter->data;
      if (WINDOW_EDGES_RELEVANT (cur_window, display))
        {
          MetaRectangle *new_rect;
          new_rect = g_new (MetaRectangle, 1);
          meta_window_get_outer_rect (cur_window, new_rect);
          obscuring_windows = g_slist_prepend (obscuring_windows, new_rect);
          window_stacking = 
            g_slist_prepend (window_stacking, GINT_TO_POINTER (stack_position));
        }

      stack_position++;
      cur_window_iter = cur_window_iter->next;
    }
  /* Put 'em in bottom to top order */
  rem_windows       = g_slist_reverse (obscuring_windows);
  rem_win_stacking  = g_slist_reverse (window_stacking);

  /*
   * 3rd: loop over the windows again, this time getting the edges from
   * them and removing intersections with the relevant obscuring_windows &
   * obscuring_docks.
   */
  edges = NULL;
  stack_position = 0;
  cur_window_iter = stacked_windows;
  while (cur_window_iter != NULL)
    {
      MetaRectangle  cur_rect;
      MetaWindow    *cur_window = cur_window_iter->data;
      meta_window_get_outer_rect (cur_window, &cur_rect);

      /* Check if we want to use this window's edges for edge
       * resistance (note that dock edges are considered screen edges
       * which are handled separately
       */
      if (WINDOW_EDGES_RELEVANT (cur_window, display) &&
          cur_window->type != META_WINDOW_DOCK)
        {
          GList *new_edges;
          MetaEdge *new_edge;
          MetaRectangle reduced;

          /* We don't care about snapping to any portion of the window that
           * is offscreen (we also don't care about parts of edges covered
           * by other windows or DOCKS, but that's handled below).
           */
          meta_rectangle_intersect (&cur_rect, 
                                    &display->grab_screen->rect,
                                    &reduced);

          new_edges = NULL;

          /* Left side of this window is resistance for the right edge of
           * the window being moved.
           */
          new_edge = g_new (MetaEdge, 1);
          new_edge->rect = reduced;
          new_edge->rect.width = 0;
          new_edge->side_type = META_DIRECTION_RIGHT;
          new_edge->edge_type = META_EDGE_WINDOW;
          new_edges = g_list_prepend (new_edges, new_edge);

          /* Right side of this window is resistance for the left edge of
           * the window being moved.
           */
          new_edge = g_new (MetaEdge, 1);
          new_edge->rect = reduced;
          new_edge->rect.x += new_edge->rect.width;
          new_edge->rect.width = 0;
          new_edge->side_type = META_DIRECTION_LEFT;
          new_edge->edge_type = META_EDGE_WINDOW;
          new_edges = g_list_prepend (new_edges, new_edge);
          
          /* Top side of this window is resistance for the bottom edge of
           * the window being moved.
           */
          new_edge = g_new (MetaEdge, 1);
          new_edge->rect = reduced;
          new_edge->rect.height = 0;
          new_edge->side_type = META_DIRECTION_BOTTOM;
          new_edge->edge_type = META_EDGE_WINDOW;
          new_edges = g_list_prepend (new_edges, new_edge);

          /* Top side of this window is resistance for the bottom edge of
           * the window being moved.
           */
          new_edge = g_new (MetaEdge, 1);
          new_edge->rect = reduced;
          new_edge->rect.y += new_edge->rect.height;
          new_edge->rect.height = 0;
          new_edge->side_type = META_DIRECTION_TOP;
          new_edge->edge_type = META_EDGE_WINDOW;
          new_edges = g_list_prepend (new_edges, new_edge);

          /* Update the remaining windows to only those at a higher
           * stacking position than this one.
           */
          while (rem_win_stacking && 
                 stack_position >= (int)rem_win_stacking->data)
            {
              rem_windows      = rem_windows->next;
              rem_win_stacking = rem_win_stacking->next;
            }

          /* Remove edge portions overlapped by rem_windows and rem_docks */
          new_edges = 
            meta_rectangle_remove_intersections_with_boxes_from_edges (
              new_edges,
              rem_windows);

          /* Save the new edges */
          edges = g_list_concat (new_edges, edges);
        }

      stack_position++;
      cur_window_iter = cur_window_iter->next;
    }

  /*
   * 4th: Free the extra memory not needed and sort the list
   */
  /* Free the memory used by the obscuring windows/docks lists */
  g_slist_free (window_stacking);
  /* FIXME: Shouldn't there be a helper function to make this one line of code
   * to free a list instead of four ugly ones?
   */
  g_slist_foreach (obscuring_windows, 
                   (void (*)(gpointer,gpointer))&g_free, /* ew, for ugly */
                   NULL);
  g_slist_free (obscuring_windows);

  /* Sort the list.  FIXME: Should I bother with this sorting?  I just
   * sort again later in cache_edges() anyway...
   */
  edges = g_list_sort (edges, meta_rectangle_edge_cmp);

  /*
   * 5th: Cache the combination of these edges with the onscreen and
   * xinerama edges in an array for quick access.  Free the edges since
   * they've been cached elsewhere.
   */
  cache_edges (display,
               edges,
               display->grab_screen->active_workspace->xinerama_edges,
               display->grab_screen->active_workspace->screen_edges);
  g_list_free (edges);

  /*
   * 6th: Initialize the resistance timeouts and buildups
   */
  initialize_grab_edge_resistance_data (display);
}

/* Note that old_[xy] and new_[xy] are with respect to inner positions of
 * the window.
 */
void
meta_window_edge_resistance_for_move (MetaWindow  *window,
                                      int          old_x,
                                      int          old_y,
                                      int         *new_x,
                                      int         *new_y,
                                      GSourceFunc  timeout_func,
                                      gboolean     snap,
                                      gboolean     is_keyboard_op)
{
  MetaRectangle old_outer, proposed_outer, new_outer;

  if (window == window->display->grab_window &&
      window->display->grab_wireframe_active)
    {
      meta_window_get_xor_rect (window,
                                &window->display->grab_wireframe_rect,
                                &old_outer);
    }
  else
    {
      meta_window_get_outer_rect (window, &old_outer);
    }
  proposed_outer = old_outer;
  proposed_outer.x += (*new_x - old_x);
  proposed_outer.y += (*new_y - old_y);
  new_outer = proposed_outer;

  window->display->grab_last_user_action_was_snap = snap;
  if (apply_edge_resistance_to_each_side (window->display,
                                          window,
                                          &old_outer,
                                          &new_outer,
                                          timeout_func,
                                          snap,
                                          is_keyboard_op))
    {
      /* apply_edge_resistance_to_each_side independently applies
       * resistance to both the right and left edges of new_outer as both
       * could meet areas of resistance.  But we don't want a resize, so we
       * just have both edges move according to the stricter of the
       * resistances.  Same thing goes for top & bottom edges.
       */
      MetaRectangle *reference;
      int left_change, right_change, smaller_x_change;
      int top_change, bottom_change, smaller_y_change;

      if (snap && !is_keyboard_op)
        reference = &proposed_outer;
      else
        reference = &old_outer;

      left_change  = BOX_LEFT (new_outer)  - BOX_LEFT (*reference);
      right_change = BOX_RIGHT (new_outer) - BOX_RIGHT (*reference);
      if (     snap && is_keyboard_op && left_change == 0)
        smaller_x_change = right_change;
      else if (snap && is_keyboard_op && right_change == 0)
        smaller_x_change = left_change;
      else if (ABS (left_change) < ABS (right_change))
        smaller_x_change = left_change;
      else
        smaller_x_change = right_change;

      top_change    = BOX_TOP (new_outer)    - BOX_TOP (*reference);
      bottom_change = BOX_BOTTOM (new_outer) - BOX_BOTTOM (*reference);
      if (     snap && is_keyboard_op && top_change == 0)
        smaller_y_change = bottom_change;
      else if (snap && is_keyboard_op && bottom_change == 0)
        smaller_y_change = top_change;
      else if (ABS (top_change) < ABS (bottom_change))
        smaller_y_change = top_change;
      else
        smaller_y_change = bottom_change;

      *new_x = old_x + smaller_x_change + 
              (BOX_LEFT (*reference) - BOX_LEFT (old_outer));
      *new_y = old_y + smaller_y_change +
              (BOX_TOP (*reference) - BOX_TOP (old_outer));
    }
}

/* Note that old_(width|height) and new_(width|height) are with respect to
 * sizes of the inner window.
 */
void
meta_window_edge_resistance_for_resize (MetaWindow  *window,
                                        int          old_width,
                                        int          old_height,
                                        int         *new_width,
                                        int         *new_height,
                                        int          gravity,
                                        GSourceFunc  timeout_func,
                                        gboolean     snap,
                                        gboolean     is_keyboard_op)
{
  MetaRectangle old_outer, new_outer;
  int new_outer_width, new_outer_height;

  if (window == window->display->grab_window &&
      window->display->grab_wireframe_active)
    {
      meta_window_get_xor_rect (window,
                                &window->display->grab_wireframe_rect,
                                &old_outer);
    }
  else
    {
      meta_window_get_outer_rect (window, &old_outer);
    }
  new_outer_width  = old_outer.width  + (*new_width  - old_width);
  new_outer_height = old_outer.height + (*new_height - old_height);
  meta_rectangle_resize_with_gravity (&old_outer, 
                                      &new_outer,
                                      gravity,
                                      new_outer_width,
                                      new_outer_height);

  window->display->grab_last_user_action_was_snap = snap;
  if (apply_edge_resistance_to_each_side (window->display,
                                          window,
                                          &old_outer,
                                          &new_outer,
                                          timeout_func,
                                          snap,
                                          is_keyboard_op))
    {
      *new_width  = old_width  + (new_outer.width  - old_outer.width);
      *new_height = old_height + (new_outer.height - old_outer.height);
    }
}