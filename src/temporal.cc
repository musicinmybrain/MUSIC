/*
 *  This file is part of MUSIC.
 *  Copyright (C) 2008, 2009 INCF
 *
 *  MUSIC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  MUSIC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "music/temporal.hh"  // Must be included first on BG/L
//#define MUSIC_DEBUG

#if MUSIC_USE_MPI

#include "music/application.hh"

namespace MUSIC
{

  TemporalNegotiator::TemporalNegotiator (const Application& app) :
      app_ (app), negotiationBuffer (NULL)
  {
    nApplications_ = app_.applicationMap ().size ();
    nAllConnections = 0;
    nodes = new TemporalNegotiatorGraph (app_.timebase (), nApplications_,
        app_.color ());
  }


  TemporalNegotiator::~TemporalNegotiator ()
  {
    delete nodes;
    freeNegotiationData (negotiationBuffer);

    // Free negotiation communicator in application leaders
    if (negotiationComm != MPI::COMM_NULL)
      negotiationComm.Free ();

    applicationLeaders.Free ();
    groupWorld.Free ();
  }


  void
  TemporalNegotiator::separateConnections (
      std::vector<Connection*>* connections)
  {
    for (std::vector<Connection*>::iterator c = connections->begin ();
        c != connections->end (); ++c)
      {
        if (dynamic_cast<OutputConnection*> (*c) != NULL)
          outputConnections.push_back (*dynamic_cast<OutputConnection*> (*c));
        else
          inputConnections.push_back (*dynamic_cast<InputConnection*> (*c));
      }
  }


  TemporalNegotiatorGraph*
  TemporalNegotiator::applicationGraph ()
  {
    return nodes;
  }


  bool
  TemporalNegotiator::isLeader ()
  {
    return app_.communicator ().Get_rank () == 0;
  }


  bool
  TemporalNegotiator::hasPeers ()
  {
    return app_.communicator ().Get_size () > 1;
  }


  void
  TemporalNegotiator::createNegotiationCommunicator ()
  {
    auto applicationMap = app_.applicationMap ();
    int* ranks = new int[nApplications_];

    for (int i = 0; i < nApplications_; ++i)
      ranks[i] = applicationMap[i].leader ();

    groupWorld = MPI::COMM_WORLD.Get_group ();
    applicationLeaders = groupWorld.Incl (nApplications_, ranks);
    delete[] ranks;
    negotiationComm = MPI::COMM_WORLD.Create (applicationLeaders);
  }


  int
  TemporalNegotiator::negotiationDataSize (int nConnections)
  {
    return negotiationDataSize (1, nConnections);
  }


  int
  TemporalNegotiator::negotiationDataSize (int nBlocks, int nConnections)
  {
    // -1 due to definition of connection member
    return (nBlocks * sizeof(TemporalNegotiationData)
        + (nConnections - 1) * sizeof(ConnectionDescriptor));
  }


  TemporalNegotiationData*
  TemporalNegotiator::allocNegotiationData (int nBlocks, int nConnections)
  {
    void* memory = new char[negotiationDataSize (nBlocks, nConnections)];
    return static_cast<TemporalNegotiationData*> (memory);
  }


  void
  TemporalNegotiator::freeNegotiationData (TemporalNegotiationData* data)
  {
    if (data != NULL)
      {
        delete[] static_cast<char*> (static_cast<void*> (data));
        data = NULL;
      }
  }


  int
  TemporalNegotiator::computeDefaultMaxBuffered (int maxLocalWidth,
      int eventSize, ClockState tickInterval, double timebase)
  {
    int res;

    // Do different calculation depending on the kind of port

    // NOTE: We should not base this choice eventSize but instead on
    // proper type information

    if (eventSize == 0)
      // continuous data
      res = DEFAULT_PACKET_SIZE / maxLocalWidth;
    else if (eventSize == 1)
      // message data
      res = DEFAULT_MESSAGE_MAX_BUFFERED;
    else
      // event data
      res = (DEFAULT_PACKET_SIZE
          / (EVENT_FREQUENCY_ESTIMATE * maxLocalWidth * eventSize * timebase
              * tickInterval));
    if (res < 1)
      res = 1;
    return res;
  }


  int
  TemporalNegotiator::findNodeColor (int leader)
  {
    int color = -1;
    auto applicationMap = app_.applicationMap ();
    for (int i = 0; i < nApplications_; ++i)
      if (leader == applicationMap[i].leader ())
        color = applicationMap[i].color ();
    assert (color != -1);
    return color;
  }


  void
  TemporalNegotiator::collectNegotiationData (ClockState ti)
  {
    int nOut = outputConnections.size ();
    int nIn = inputConnections.size ();
    nLocalConnections = nOut + nIn;
    MUSIC_LOG ("nLocalConnections = " << nLocalConnections
        << ", nOut = " << nOut
        << ", nIn = " << nIn);
    negotiationData = allocNegotiationData (1, nLocalConnections);
    negotiationData->timebase = app_.timebase ();
    negotiationData->tickInterval = ti;
    negotiationData->color = app_.color ();
    negotiationData->leader = app_.leader ();
    negotiationData->nProcs = app_.nProcs ();
    negotiationData->nOutConnections = outputConnections.size ();
    negotiationData->nInConnections = inputConnections.size ();

    for (int i = 0; i < nOut; ++i)
      {
        Connector* connector = outputConnections[i].connector ();
        int remote = connector->remoteLeader ();
        negotiationData->connection[i].remoteNode = findNodeColor (remote);
        negotiationData->connection[i].receiverPort =
            connector->receiverPortCode ();
        negotiationData->connection[i].multiComm = connector->idFlag ();
        negotiationData->connection[i].maxBuffered =
            outputConnections[i].maxBuffered ();
        negotiationData->connection[i].defaultMaxBuffered =
            computeDefaultMaxBuffered (connector->maxLocalWidth (),
                outputConnections[i].elementSize (), ti, app_.timebase ());

        negotiationData->connection[i].accLatency = 0;
      }
    for (int i = 0; i < nIn; ++i)
      {
        int remote = inputConnections[i].connector ()->remoteLeader ();
        negotiationData->connection[nOut + i].remoteNode = findNodeColor (
            remote);
        negotiationData->connection[nOut + i].receiverPort =
            inputConnections[i].connector ()->receiverPortCode ();
        negotiationData->connection[nOut + i].multiComm =
            inputConnections[i].connector ()->idFlag ();
        negotiationData->connection[nOut + i].maxBuffered =
            inputConnections[i].maxBuffered ();
        negotiationData->connection[nOut + i].defaultMaxBuffered = 0;

        negotiationData->connection[nOut + i].accLatency =
            inputConnections[i].accLatency ();
        MUSIC_LOGR ("port " << inputConnections[i].connector ()->receiverPortName () << ": " << inputConnections[i].accLatency ());
        negotiationData->connection[nOut + i].interpolate =
            inputConnections[i].interpolate ();
      }
  }


  void
  TemporalNegotiator::fillTemporalNegotiatorGraph ()
  {
    nodes->setNConnections (nAllConnections / 2);
    char* memory = static_cast<char*> (static_cast<void*> (negotiationBuffer));
    TemporalNegotiationData* data;
    for (int i = 0; i < nApplications_; ++i)
      {

        data =
            static_cast<TemporalNegotiationData*> (static_cast<void*> (memory));
        int nOut = data->nOutConnections;
        int nIn = data->nInConnections;
        nodes->addNode (
            ANode<TemporalNegotiationData, ConnectionDescriptor> (nOut, nIn,
                *data), data->color);
        memory += negotiationDataSize (nOut + nIn);
      }

  }


  void
  TemporalNegotiator::communicateNegotiationData ()
  {
    // First talk to others about how many connections each node has
    int* nConnections = new int[nApplications_];
    negotiationComm.Allgather (&nLocalConnections, 1, MPI::INT, nConnections, 1,
        MPI::INT);
    for (int i = 0; i < nApplications_; ++i)
      nAllConnections += nConnections[i];
    negotiationBuffer = allocNegotiationData (nApplications_, nAllConnections);
    int* receiveSizes = new int[nApplications_];
    int* displacements = new int[nApplications_];
    int displacement = 0;
    for (int i = 0; i < nApplications_; ++i)
      {
        int receiveSize = negotiationDataSize (nConnections[i]);
        receiveSizes[i] = receiveSize;
        displacements[i] = displacement;

        displacement += receiveSize;
      }
    delete[] nConnections;
    int sendSize = negotiationDataSize (nLocalConnections);
    negotiationComm.Allgatherv (negotiationData, sendSize, MPI::BYTE,
        negotiationBuffer, receiveSizes, displacements, MPI::BYTE);
    delete[] displacements;
    delete[] receiveSizes;
    freeNegotiationData (negotiationData);
    fillTemporalNegotiatorGraph ();
  }


  ConnectionDescriptor*
  TemporalNegotiator::findInputConnection (int node, int port)
  {
    int nOut = nodes->at (node).data ().nOutConnections;
    // Get address of first input ConnectionDescriptor
    ConnectionDescriptor* inputDescriptors =
        &nodes->at (node).data ().connection[nOut];
    for (int i = 0; i < nodes->at (node).data ().nInConnections; ++i)
      if (inputDescriptors[i].receiverPort == port)
        return &inputDescriptors[i];
    error ("internal error in TemporalNegotiator::findInputConnection");
    return 0; // never reached
  }


  void
  TemporalNegotiator::combineParameters ()
  {
    double timebase = nodes->at (0).data ().timebase;

    for (int o = 0; o < nApplications_; ++o)
      {
        // check timebase
        if (nodes->at (o).data ().timebase != timebase)
          error0 ("applications don't use same timebase");

        for (int c = 0; c < nodes->at (o).data ().nOutConnections; ++c)
          {
            ConnectionDescriptor* out = &nodes->at (o).data ().connection[c];
            int i = out->remoteNode;
            ConnectionDescriptor* in = findInputConnection (i,
                out->receiverPort);

            // maxBuffered

            // check defaults
            if (out->maxBuffered == MAX_BUFFERED_NO_VALUE
                && in->maxBuffered == MAX_BUFFERED_NO_VALUE)
              out->maxBuffered = out->defaultMaxBuffered;
            else if (in->maxBuffered != MAX_BUFFERED_NO_VALUE)
              {
                // convert to sender side ticks
                ClockState inMaxBufferedTime = in->maxBuffered
                    * nodes->at (i).data ().tickInterval;
                int inMaxBuffered = (inMaxBufferedTime
                    / nodes->at (o).data ().tickInterval);
                // take min maxBuffered
                if (out->maxBuffered == MAX_BUFFERED_NO_VALUE
                    || inMaxBuffered < out->maxBuffered)
                  out->maxBuffered = inMaxBuffered;
              }
            // store maxBuffered in sender units
            in->maxBuffered = out->maxBuffered;
            MUSIC_LOG0("Max Buffered:"<<in->maxBuffered);

            // accLatency
            out->accLatency = in->accLatency;

            // interpolate
            out->interpolate = in->interpolate;

            // remoteTickInterval
            out->remoteTickInterval = nodes->at (i).data ().tickInterval;
            in->remoteTickInterval = nodes->at (o).data ().tickInterval;
          }
      }
  }


  void
  TemporalNegotiator::distributeParameters ()
  {
    for (int o = 0; o < nApplications_; ++o)
      {
        for (int c = 0; c < nodes->at (o).data ().nOutConnections; ++c)
          {
            ConnectionDescriptor* out = &nodes->at (o).data ().connection[c];
            int i = out->remoteNode;
            ConnectionDescriptor* in = findInputConnection (i,
                out->receiverPort);

            // store maxBuffered in sender units
            in->maxBuffered = out->maxBuffered;
          }
      }
  }


  void
  TemporalNegotiator::loopAlgorithm ()
  {
    std::vector<AEdge<TemporalNegotiationData, ConnectionDescriptor> > path;
    TemporalNegotiatorGraph::iterator it;
    for (it = nodes->begin (); it < nodes->end (); ++it)
      if (!it->visited)
        nodes->depthFirst (*it, path);
  }


  void
  TemporalNegotiator::broadcastNegotiationData ()
  {
    MPI::Intracomm comm = app_.communicator ();

    if (hasPeers ())
      {
        comm.Bcast (&nAllConnections, 1, MPI::INT, 0);
        char* memory =
            static_cast<char*> (static_cast<void*> (negotiationBuffer));
        comm.Bcast (memory,
            negotiationDataSize (nApplications_, nAllConnections), MPI::BYTE,
            0);
      }
  }


  void
  TemporalNegotiator::receiveNegotiationData ()
  {
    MPI::Intracomm comm = app_.communicator ();
    comm.Bcast (&nAllConnections, 1, MPI::INT, 0);
    negotiationBuffer = allocNegotiationData (nApplications_, nAllConnections);
    char* memory = static_cast<char*> (static_cast<void*> (negotiationBuffer));
    comm.Bcast (memory, negotiationDataSize (nApplications_, nAllConnections),
        MPI::BYTE, 0);
    fillTemporalNegotiatorGraph ();
  }


  void
  TemporalNegotiator::negotiate (Clock& localTime,
      std::vector<Connection*>* connections)
  {
    separateConnections (connections);

    //MUSIC_LOGR (printconns ("NOut: ", outputConnections));
    //MUSIC_LOGR (printconns ("NIn: ", inputConnections));
    createNegotiationCommunicator ();

    if (isLeader ())
      {
        collectNegotiationData (localTime.tickInterval ());
        communicateNegotiationData ();
        combineParameters ();
        loopAlgorithm ();
        distributeParameters ();
        broadcastNegotiationData ();
      }
    else
      receiveNegotiationData ();
  }


  void
  TemporalNegotiatorGraph::handleLoop (
      ANode<TemporalNegotiationData, ConnectionDescriptor> &x,
      std::vector<AEdge<TemporalNegotiationData, ConnectionDescriptor> > & path)
  {
    // Search path to detect beginning of loop
    int loop;
    for (loop = 0; &path[loop].pre () != &x; ++loop)
      ;
    // Compute how much headroom we have for buffering
    ClockState totalDelay = 0;
    for (unsigned int c = loop; c < path.size (); ++c)
      {

        MUSIC_LOGR ("latency = " << path[c].data().accLatency << ", ti = "
            << path[c].pre ().data().tickInterval);
        totalDelay += path[c].data ().accLatency
            - path[c].pre ().data ().tickInterval;
      }

    // If negative we will not be able to make it in time
    // around the loop even without any extra buffering
    if (totalDelay < 0)
      {
        std::ostringstream ostr;
        ostr << "too short latency (" << -timebase_ * totalDelay
            << " s) around loop: "; // << path[loop].pre().name();
        for (unsigned int c = loop + 1; c < path.size (); ++c)
          ostr << ", "; // << path[c].pre().name();
        error0 (ostr.str ());
      }

    // Distribute totalDelay as allowed buffering uniformly over loop
    // (we could do better by considering constraints form other loops)
    int loopLength = path.size () - loop;
    ClockState bufDelay = totalDelay / loopLength;
    for (unsigned int c = loop; c < path.size (); ++c)
      {
        int allowedTicks = bufDelay / path[c].pre ().data ().tickInterval;
        path[c].data ().maxBuffered = std::min (path[c].data ().maxBuffered,
            allowedTicks);
      }
  }


  void
  TemporalNegotiatorGraph::setNConnections (int nConns)
  {
    nConnections_ = nConns;
  }


  int
  TemporalNegotiatorGraph::getNConnections ()
  {
    return nConnections_;
  }
}
#endif
