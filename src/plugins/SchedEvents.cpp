// SPDX-License-Identifier: LGPL-2.1

/*
 * Copyright (C) 2018 VMware Inc, Yordan Karadzhov (VMware) <y.karadz@gmail.com>
 */

/**
 *  @file    SchedEvents.cpp
 *  @brief   Defines a callback function for Sched events used to plot in green
 *	     the wake up latency of the task and in red the time the task was
 *	     preempted by another task.
 */

// C++
#include <vector>

// KernelShark
#include "libkshark.h"
#include "libkshark-plugin.h"
#include "plugins/sched_events.h"
#include "KsPlotTools.hpp"
#include "KsPlugins.hpp"
#include "KsMainWindow.hpp"

using namespace KsPlot;

static KsMainWindow *ks_ptr;

/**
 * @brief Provide the plugin with a pointer to the KsMainWindow object (the GUI
 * itself) such that the plugin can manipulate the GUI.
 */
void *plugin_set_gui_ptr(void *gui_ptr)
{
	ks_ptr = static_cast<KsMainWindow *>(gui_ptr);
	return nullptr;
}

/**
 * This class represents the graphical element visualizing the latency between
 *  sched_waking and sched_switch events.
 */
class LatencyBox : public Rectangle
{
	/** On double click do. */
	void _doubleClick() const override
	{
		ks_ptr->markEntry(_data[1]->entry, DualMarkerState::B);
		ks_ptr->markEntry(_data[0]->entry, DualMarkerState::A);
	}

public:
	/** The trace record data that corresponds to this LatencyBox. */
	std::vector<kshark_data_field_int64 *>	_data;

	/**
	 * @brief Distance between the click and the shape. Used to decide if
	 *	  the double click action must be executed.
	 *
	 * @param x: X coordinate of the click.
	 * @param y: Y coordinate of the click.
	 *
	 * @returns If the click is inside the box, the distance is zero.
	 *	    Otherwise infinity.
	 */
	double distance(int x, int y) const override
	{
		if (x < pointX(0) || x > pointX(2))
			return std::numeric_limits<double>::max();

		if (y < pointY(0) || y > pointY(1))
			return std::numeric_limits<double>::max();

		return 0;
	}
};

static PlotObject *makeShape(std::vector<const Graph *> graph,
			     std::vector<int> bins,
			     std::vector<kshark_data_field_int64 *> data,
			     Color col, float size)
{
	LatencyBox *rec = new LatencyBox;
	rec->_data = data;

	Point p0 = graph[0]->bin(bins[0])._base;
	Point p1 = graph[0]->bin(bins[1])._base;
	int height = graph[0]->height() * .3;

	rec->setFill(false);
	rec->setPoint(0, p0.x() - 1, p0.y() - height);
	rec->setPoint(1, p0.x() - 1, p0.y() - 1);

	rec->setPoint(3, p1.x() - 1, p1.y() - height);
	rec->setPoint(2, p1.x() - 1, p1.y() - 1);

	rec->_size = size;
	rec->_color = col;

	return rec;
};

/*
 * Ideally, the sched_switch has to be the last trace event recorded before the
 * task is preempted. Because of this, when the data is loaded (the first pass),
 * the "pid" field of the sched_switch entries gets edited by this plugin to be
 * equal to the "next pid" of the sched_switch event. However, in reality the
 * sched_switch event may be followed by some trailing events from the same task
 * (printk events for example). This has the effect of extending the graph of
 * the task outside of the actual duration of the task. The "second pass" over
 * the data is used to fix this problem. It takes advantage of the "next" field
 * of the entry (this field is set during the first pass) to search for trailing
 * events after the "sched_switch".
 */
static void secondPass(plugin_sched_context *plugin_ctx)
{
	kshark_data_container *cSS;
	kshark_entry *e;
	int pid_rec;

	cSS = plugin_ctx->ss_data;
	for (ssize_t i = 0; i < cSS->size; ++i) {
		pid_rec = plugin_sched_get_pid(cSS->data[i]->field);
		e = cSS->data[i]->entry;
		if (!e->next || e->pid == 0 ||
		    e->event_id == e->next->event_id ||
		    pid_rec != e->next->pid)
			continue;

		/* Find the very last trailing event. */
		for (; e->next; e = e->next) {
			if (e->next->pid != plugin_sched_get_pid(cSS->data[i]->field)) {
				/*
				 * This is the last trailing event. Change the
				 * "pid" to be equal to the "next pid" of the
				 * sched_switch event and leave a sign that you
				 * edited this entry.
				 */
				e->pid = cSS->data[i]->entry->pid;
				e->visible &= ~KS_PLUGIN_UNTOUCHED_MASK;
				break;
			}
		}
	}
}

/**
 * @brief Plugin's draw function.
 *
 * @param argv_c: A C pointer to be converted to KsCppArgV (C++ struct).
 * @param sd: Data stream identifier.
 * @param pid: Process Id.
 * @param draw_action: Draw action identifier.
 */
void plugin_draw(kshark_cpp_argv *argv_c, int sd, int pid, int draw_action)
{
	plugin_sched_context *plugin_ctx;

	if (!(draw_action & KSHARK_TASK_DRAW) || pid == 0)
		return;

	plugin_ctx = __get_context(sd);
	if (!plugin_ctx)
		return;

	KsCppArgV *argvCpp = KS_ARGV_TO_CPP(argv_c);

	if (!plugin_ctx->second_pass_done) {
		/* The second pass is not done yet. */
		secondPass(plugin_ctx);
		plugin_ctx->second_pass_done = true;
	}

	IsApplicableFunc checkFieldSW = [=] (kshark_data_container *d,
					     ssize_t i) {
		return d->data[i]->field == pid;
	};

	IsApplicableFunc checkFieldSS = [=] (kshark_data_container *d,
					     ssize_t i) {
		return !(plugin_sched_get_prev_state(d->data[i]->field) & 0x7f) &&
		       plugin_sched_get_pid(d->data[i]->field) == pid;
	};

	IsApplicableFunc checkEntryPid = [=] (kshark_data_container *d,
					      ssize_t i) {
		return d->data[i]->entry->pid == pid;
	};

	eventFieldIntervalPlot(argvCpp,
			       plugin_ctx->sw_data, checkFieldSW,
			       plugin_ctx->ss_data, checkEntryPid,
			       makeShape,
			       {0, 255, 0}, // Green
			       -1);         // Default size

	eventFieldIntervalPlot(argvCpp,
			       plugin_ctx->ss_data, checkFieldSS,
			       plugin_ctx->ss_data, checkEntryPid,
			       makeShape,
			       {255, 0, 0}, // Red
			       -1);         // Default size
}
