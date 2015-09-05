/*
 * NeighborRoutingExtension.cpp
 *
 * Copyright (C) 2011 IBR, TU Braunschweig
 *
 * Written-by: Johannes Morgenroth <morgenroth@ibr.cs.tu-bs.de>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "config.h"
#include "routing/NeighborRoutingExtension.h"
#include "routing/QueueBundleEvent.h"
#include "core/BundleCore.h"
#include "net/TransferCompletedEvent.h"
#include "net/TransferAbortedEvent.h"
#include "net/ConnectionEvent.h"
#include "core/NodeEvent.h"
#include "core/Node.h"
#include "net/ConnectionManager.h"
#include "ibrcommon/thread/MutexLock.h"
#include "storage/BundleStorage.h"
#include "core/BundleEvent.h"
#include <ibrcommon/Logger.h>

#ifdef HAVE_SQLITE
#include "storage/SQLiteBundleStorage.h"
#endif

#include <functional>
#include <list>
#include <algorithm>
#include <typeinfo>
#include <memory>

namespace dtn
{
	namespace routing
	{
		const std::string NeighborRoutingExtension::TAG = "NeighborRoutingExtension";

		NeighborRoutingExtension::NeighborRoutingExtension()
		{
		}

		NeighborRoutingExtension::~NeighborRoutingExtension()
		{
			join();
		}

		void NeighborRoutingExtension::__cancellation() throw ()
		{
			_taskqueue.abort();
		}

		void NeighborRoutingExtension::run() throw ()
		{
#ifdef HAVE_SQLITE
			class BundleFilter : public dtn::storage::BundleSelector, public dtn::storage::SQLiteDatabase::SQLBundleQuery
#else
			class BundleFilter : public dtn::storage::BundleSelector
#endif
			{
			public:
				BundleFilter(NeighborRoutingExtension &e, const NeighborDatabase::NeighborEntry &entry, const dtn::net::ConnectionManager::protocol_list &plist)
				 : _extension(e), _entry(entry), _plist(plist)
				{};

				virtual ~BundleFilter() {};

				virtual dtn::data::Size limit() const throw () { return _entry.getFreeTransferSlots(); };

				virtual bool addIfSelected(dtn::storage::BundleResult &result, const dtn::data::MetaBundle &meta) const throw (dtn::storage::BundleSelectorException)
				{
					// check if the considered bundle should get routed
					std::pair<bool, dtn::core::Node::Protocol> ret = _extension.shouldRouteTo(meta, _entry, _plist);

					// put the considered bundle into the result-set if it should get routed
					if (ret.first) static_cast<RoutingResult&>(result).put(meta, ret.second);

					// signal that selection to the calling module
					return ret.first;
				};

#ifdef HAVE_SQLITE
				const std::string getWhere() const throw ()
				{
					return "destination LIKE ?";
				};

				int bind(sqlite3_stmt *st, int offset) const throw ()
				{
					const std::string d = _entry.eid.getNode().getString() + "%";
					sqlite3_bind_text(st, offset, d.c_str(), static_cast<int>(d.size()), SQLITE_TRANSIENT);
					return offset + 1;
				}
#endif

			private:
				NeighborRoutingExtension &_extension;
				const NeighborDatabase::NeighborEntry &_entry;
				const dtn::net::ConnectionManager::protocol_list &_plist;
			};

			RoutingResult list;

			while (true)
			{
				NeighborDatabase &db = (**this).getNeighborDB();

				try {
					Task *t = _taskqueue.poll();
					std::auto_ptr<Task> killer(t);

					IBRCOMMON_LOGGER_DEBUG_TAG(NeighborRoutingExtension::TAG, 5) << "processing task " << t->toString() << IBRCOMMON_LOGGER_ENDL;

					/**
					 * SearchNextBundleTask triggers a search for a bundle to transfer
					 * to another host. This Task is generated by TransferCompleted, TransferAborted
					 * and node events.
					 */
					try {
						SearchNextBundleTask &task = dynamic_cast<SearchNextBundleTask&>(*t);

						// clear the result list
						list.clear();

						// lock the neighbor database while searching for bundles
						{
							// this destination is not handles by any static route
							ibrcommon::MutexLock l(db);
							NeighborDatabase::NeighborEntry &entry = db.get(task.eid, true);

							// check if enough transfer slots available (threshold reached)
							if (!entry.isTransferThresholdReached())
								throw NeighborDatabase::NoMoreTransfersAvailable(task.eid);

							// get a list of protocols supported by both, the local BPA and the remote peer
							const dtn::net::ConnectionManager::protocol_list plist =
									dtn::core::BundleCore::getInstance().getConnectionManager().getSupportedProtocols(entry.eid);

							// create a new bundle filter
							BundleFilter filter(*this, entry, plist);

							// query an unknown bundle from the storage, the list contains max. 10 items.
							(**this).getSeeker().get(filter, list);
						}

						IBRCOMMON_LOGGER_DEBUG_TAG(NeighborRoutingExtension::TAG, 5) << "got " << list.size() << " items to transfer to " << task.eid.getString() << IBRCOMMON_LOGGER_ENDL;

						// send the bundles as long as we have resources
						for (RoutingResult::const_iterator iter = list.begin(); iter != list.end(); ++iter)
						{
							try {
								// transfer the bundle to the neighbor
								transferTo(task.eid, (*iter).first, (*iter).second);
							} catch (const NeighborDatabase::AlreadyInTransitException&) { };
						}
					} catch (const NeighborDatabase::NoMoreTransfersAvailable &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const NeighborDatabase::EntryNotFoundException &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const NodeNotAvailableException &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const dtn::storage::NoBundleFoundException &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const std::bad_cast&) { };

					/**
					 * process a received bundle
					 */
					try {
						const ProcessBundleTask &task = dynamic_cast<ProcessBundleTask&>(*t);

						// variable to store the result of shouldRouteTo()
						std::pair<bool, dtn::core::Node::Protocol> ret;

						// get a list of protocols supported by both, the local BPA and the remote peer
						const dtn::net::ConnectionManager::protocol_list plist =
								dtn::core::BundleCore::getInstance().getConnectionManager().getSupportedProtocols(task.nexthop);

						// lock the neighbor database while searching for bundles
						{
							// this destination is not handles by any static route
							ibrcommon::MutexLock l(db);
							NeighborDatabase::NeighborEntry &entry = db.get(task.nexthop, true);

							ret = shouldRouteTo(task.bundle, entry, plist);
							if (!ret.first) throw NeighborDatabase::NoRouteKnownException();
						}

						// transfer the bundle to the neighbor
						transferTo(task.nexthop, task.bundle, ret.second);
					} catch (const NeighborDatabase::AlreadyInTransitException &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const NeighborDatabase::NoMoreTransfersAvailable &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const NeighborDatabase::EntryNotFoundException &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const NodeNotAvailableException &ex) {
						IBRCOMMON_LOGGER_DEBUG_TAG(TAG, 10) << "task " << t->toString() << " aborted: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					} catch (const NeighborDatabase::NoRouteKnownException &ex) {
						// nothing to do here.
					} catch (const std::bad_cast&) { };
				} catch (const std::exception &ex) {
					IBRCOMMON_LOGGER_DEBUG_TAG(NeighborRoutingExtension::TAG, 15) << "terminated due to " << ex.what() << IBRCOMMON_LOGGER_ENDL;
					return;
				}

				yield();
			}
		}

		std::pair<bool, dtn::core::Node::Protocol> NeighborRoutingExtension::shouldRouteTo(const dtn::data::MetaBundle &meta, const NeighborDatabase::NeighborEntry &n, const dtn::net::ConnectionManager::protocol_list &plist) const
		{
			// check Scope Control Block - do not forward bundles with hop limit == 0
			if (meta.hopcount == 0)
			{
				return make_pair(false, dtn::core::Node::CONN_UNDEFINED);
			}

			if (meta.get(dtn::data::PrimaryBlock::DESTINATION_IS_SINGLETON))
			{
				// do not forward local bundles
				if (meta.destination.sameHost(dtn::core::BundleCore::local))
				{
					return make_pair(false, dtn::core::Node::CONN_UNDEFINED);
				}

				// do not forward bundles for other nodes
				if (!meta.destination.sameHost(n.eid))
				{
					return make_pair(false, dtn::core::Node::CONN_UNDEFINED);
				}
			}
			else
			{
				// do not forward non-singleton bundles
				return make_pair(false, dtn::core::Node::CONN_UNDEFINED);
			}

			// do not forward bundles already known by the destination
			if (n.has(meta))
			{
				return make_pair(false, dtn::core::Node::CONN_UNDEFINED);
			}

			// update filter context
			dtn::core::FilterContext context;
			context.setPeer(n.eid);
			context.setRouting(*this);
			context.setMetaBundle(meta);

			// check bundle filter for each possible path
			for (dtn::net::ConnectionManager::protocol_list::const_iterator it = plist.begin(); it != plist.end(); ++it)
			{
				const dtn::core::Node::Protocol &p = (*it);

				// update context with current protocol
				context.setProtocol(p);

				// execute filtering
				dtn::core::BundleFilter::ACTION ret = dtn::core::BundleCore::getInstance().evaluate(dtn::core::BundleFilter::ROUTING, context);

				if (ret == dtn::core::BundleFilter::ACCEPT)
				{
					// put the selected bundle with targeted interface into the result-set
					return make_pair(true, p);
				}
			}

			return make_pair(false, dtn::core::Node::CONN_UNDEFINED);
		}

		void NeighborRoutingExtension::eventDataChanged(const dtn::data::EID &peer) throw ()
		{
			// transfer the next bundle to this destination
			_taskqueue.push( new SearchNextBundleTask( peer ) );
		}

		void NeighborRoutingExtension::eventBundleQueued(const dtn::data::EID &peer, const dtn::data::MetaBundle &meta) throw ()
		{
			// try to deliver new bundles to all neighbors
			const std::set<dtn::core::Node> nl = dtn::core::BundleCore::getInstance().getConnectionManager().getNeighbors();

			for (std::set<dtn::core::Node>::const_iterator iter = nl.begin(); iter != nl.end(); ++iter)
			{
				const dtn::core::Node &n = (*iter);

				if (n.getEID() != peer) {
					// transfer the next bundle to this destination
					_taskqueue.push( new ProcessBundleTask(meta, peer, n.getEID()) );
				}
			}
		}

		void NeighborRoutingExtension::componentUp() throw ()
		{
			// reset the task queue
			_taskqueue.reset();

			// routine checked for throw() on 15.02.2013
			try {
				// run the thread
				start();
			} catch (const ibrcommon::ThreadException &ex) {
				IBRCOMMON_LOGGER_TAG(NeighborRoutingExtension::TAG, error) << "componentUp failed: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
			}
		}

		void NeighborRoutingExtension::componentDown() throw ()
		{
			// routine checked for throw() on 15.02.2013
			try {
				// stop the thread
				stop();
				join();
			} catch (const ibrcommon::ThreadException &ex) {
				IBRCOMMON_LOGGER_TAG(NeighborRoutingExtension::TAG, error) << "componentDown failed: " << ex.what() << IBRCOMMON_LOGGER_ENDL;
			}
		}

		const std::string NeighborRoutingExtension::getTag() const throw ()
		{
			return "neighbor";
		}

		/****************************************/

		NeighborRoutingExtension::SearchNextBundleTask::SearchNextBundleTask(const dtn::data::EID &e)
		 : eid(e)
		{ }

		NeighborRoutingExtension::SearchNextBundleTask::~SearchNextBundleTask()
		{ }

		std::string NeighborRoutingExtension::SearchNextBundleTask::toString()
		{
			return "SearchNextBundleTask: " + eid.getString();
		}

		/****************************************/

		NeighborRoutingExtension::ProcessBundleTask::ProcessBundleTask(const dtn::data::MetaBundle &meta, const dtn::data::EID &o, const dtn::data::EID &n)
		 : bundle(meta), origin(o), nexthop(n)
		{ }

		NeighborRoutingExtension::ProcessBundleTask::~ProcessBundleTask()
		{ }

		std::string NeighborRoutingExtension::ProcessBundleTask::toString()
		{
			return "ProcessBundleTask: " + bundle.toString();
		}
	}
}
