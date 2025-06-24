const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const jwt = require('jsonwebtoken');
const { createClient } = require('redis');
const { createAdapter } = require('@socket.io/redis-adapter');
const WebSocket = require('ws');

const PORT = 3456;
const WS_PORT = 3457;

const app = express();
const server = http.createServer(app);
const io = new Server(server, { 
  cors: { origin: '*' },
  path: '/socket.io/'  // Explicitly set Socket.IO path to avoid conflict with /websocket
});

// Secret key for JWT (store in environment variables in production)
const JWT_SECRET = process.env.JWT_SECRET || 'your-secure-secret-key';

console.log('Creating Redis clients...');

// Redis clients for pub/sub and storage
const pubClient = createClient({ url: 'redis://localhost:6379' });
const subClient = pubClient.duplicate();

// Store for raw WebSocket connections
const rawWebSocketClients = new Map(); // socketId -> { ws, userId, room }

pubClient.on('error', err => {
  console.log('Redis Pub Client Error:', err);
  process.exit(1);
});
subClient.on('error', err => {
  console.log('Redis Sub Client Error:', err);
  process.exit(1);
});

console.log('Connecting to Redis...');

Promise.all([pubClient.connect(), subClient.connect()])
  .then(() => {
    console.log('Redis connected successfully');
    io.adapter(createAdapter(pubClient, subClient));
    console.log('Socket.IO Redis adapter configured');
  })
  .catch(err => {
    console.error('Failed to connect to Redis:', err);
    process.exit(1);
  });

// Create separate raw WebSocket server on port 3001
console.log('Creating raw WebSocket server on port ' + WS_PORT + '...');
const rawWsServer = new WebSocket.Server({ 
  port: WS_PORT,
  verifyClient: (info) => {
    // Allow all connections for now - authentication happens after connection
    return true;
  }
});

// Handle raw WebSocket connections
rawWsServer.on('connection', async (ws, req) => {
  console.log('Raw WebSocket client connected from', req.socket.remoteAddress);
  
  let clientData = null;
  const socketId = `ws_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
  
  const ip = req.socket.remoteAddress?.replace(/^::ffff:/, '');
  const port = 8888; // default DirectCallee listening port
  
  ws.on('message', async (data) => {
    const message = data.toString();
    console.log('Raw WebSocket message:', message);
    
    if (message.startsWith('REGISTER:')) {
      // Format: REGISTER:user_id:room_name
      const parts = message.split(':');
      if (parts.length >= 3) {
        const userId = parts[1];
        const room = parts[2];
        
        clientData = { ws, userId, room, socketId, ip, port };
        rawWebSocketClients.set(socketId, clientData);
        
        // Store peer in Redis
        const peerData = { userId, room, socketId, ip, port, type: 'websocket' };
        await pubClient.hSet(`room:${room}`, socketId, JSON.stringify(peerData));
        
        console.log(`Raw WebSocket user ${userId} registered in room ${room}`);
      }
    } else if (message.startsWith('HELLO:')) {
      // Format: HELLO:target_user_id
      const targetUserId = message.substring(6);
      console.log(`Raw WebSocket HELLO from ${clientData?.userId} to ${targetUserId}`);
      
      if (clientData?.room) {
        const peers = await pubClient.hGetAll(`room:${clientData.room}`);
        for (const [peerSocketId, peerDataStr] of Object.entries(peers)) {
          const peerData = JSON.parse(peerDataStr);
          if (peerData.userId === targetUserId) {
            console.log(`Routing HELLO to ${targetUserId} (socket: ${peerSocketId})`);
            
            // Send to raw WebSocket client
            if (peerData.type === 'websocket') {
              const targetClient = rawWebSocketClients.get(peerSocketId);
              if (targetClient && targetClient.ws.readyState === WebSocket.OPEN) {
                targetClient.ws.send(`HELLO:${targetUserId}`);
                console.log(`Sent HELLO to raw WebSocket client ${targetUserId}`);
              }
            } else {
              // Send to Socket.IO client
              io.to(peerSocketId).emit('message', `HELLO:${targetUserId}`);
            }

            // Also send address information back to the caller (this ws)
            if (clientData && clientData.ws.readyState === WebSocket.OPEN) {
              const addrMsg = `ADDRESS:${targetUserId}:${peerData.ip}:${peerData.port}`;
              clientData.ws.send(addrMsg);
              console.log(`Sent ${addrMsg} to caller ${clientData.userId}`);
            }
            break;
          }
        }
      }
    } else if (message === 'WELCOME') {
      console.log(`Raw WebSocket WELCOME from ${clientData?.userId}`);
      // Route WELCOME back to other clients in room
      if (clientData?.room) {
        await broadcastToRoom(clientData.room, 'WELCOME', socketId);
      }
    } else if (message === 'INIT') {
      console.log(`Raw WebSocket INIT from ${clientData?.userId}`);
      // Route INIT to other clients in room
      if (clientData?.room) {
        await broadcastToRoom(clientData.room, 'INIT', socketId);
      }
    } else if (message === 'WAITING') {
      console.log(`Raw WebSocket WAITING from ${clientData?.userId}`);
      // Route WAITING back to other clients in room
      if (clientData?.room) {
        await broadcastToRoom(clientData.room, 'WAITING', socketId);
      }
    } else if (message.startsWith('OFFER:') || message.startsWith('ANSWER:') || message.startsWith('ICE:')) {
      console.log(`Raw WebSocket ${message.split(':')[0]} from ${clientData?.userId}`);
      // Route message to other clients in room
      if (clientData?.room) {
        await broadcastToRoom(clientData.room, message, socketId);
      }
    } else if (message === 'BYE' || message === 'CANCEL') {
      console.log(`Raw WebSocket ${message} from ${clientData?.userId}`);
      // Route message to other clients in room
      if (clientData?.room) {
        await broadcastToRoom(clientData.room, message, socketId);
      }
    } else if (message === 'USER_LIST') {
      console.log(`Raw WebSocket USER_LIST request from ${clientData?.userId}`);
      if (clientData?.room) {
        const peers = await pubClient.hGetAll(`room:${clientData.room}`);
        const uniq = [...new Set(Object.values(peers).map(p => JSON.parse(p).userId))];
        const userList = uniq.filter(u => u !== clientData.userId).join(',');
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(`USER_LIST:${userList}`);
          console.log(`Sent USER_LIST:${userList} to ${clientData.userId}`);
        }
      }
    } else {
      console.log(`Unknown raw WebSocket message from ${clientData?.userId}: ${message}`);
    }
  });
  
  ws.on('close', async () => {
    console.log('Raw WebSocket client disconnected:', clientData?.userId);
    if (clientData) {
      rawWebSocketClients.delete(socketId);
      if (clientData.room) {
        await pubClient.hDel(`room:${clientData.room}`, socketId);
      }
    }
  });
  
  ws.on('error', (error) => {
    console.error('Raw WebSocket error:', error);
  });
});

console.log('Raw WebSocket server listening on port ' + WS_PORT);

// Helper function to broadcast messages to all clients in a room
async function broadcastToRoom(room, message, excludeSocketId) {
  const peers = await pubClient.hGetAll(`room:${room}`);
  for (const [peerSocketId, peerDataStr] of Object.entries(peers)) {
    if (peerSocketId === excludeSocketId) continue; // Don't send back to sender
    
    const peerData = JSON.parse(peerDataStr);
    
    if (peerData.type === 'websocket') {
      // Send to raw WebSocket client
      const targetClient = rawWebSocketClients.get(peerSocketId);
      if (targetClient && targetClient.ws.readyState === WebSocket.OPEN) {
        targetClient.ws.send(message);
        console.log(`Broadcasted "${message}" to raw WebSocket client ${peerData.userId}`);
      }
    } else {
      // Send to Socket.IO client
      io.to(peerSocketId).emit('message', message);
      console.log(`Broadcasted "${message}" to Socket.IO client ${peerData.userId}`);
    }
  }
}

// Middleware to verify JWT
io.use((socket, next) => {
  const token = socket.handshake.auth.token;
  if (!token) {
    return next(new Error('Authentication error: No token provided'));
  }
  try {
    const decoded = jwt.verify(token, JWT_SECRET);
    socket.user = decoded; // Attach user data to socket
    next();
  } catch (err) {
    next(new Error('Authentication error: Invalid token'));
  }
});

io.on('connection', async (socket) => {
  console.log('Client connected:', socket.id, 'User:', socket.user.userId);

  // Handle raw protocol messages (HELLO, INIT, WELCOME, etc.)
  socket.on('message', async (message) => {
    console.log('Raw protocol message from', socket.user.userId + ':', message);
    
    // Parse protocol messages
    if (message.startsWith('REGISTER:')) {
      // Format: REGISTER:user_id:room_name
      const parts = message.split(':');
      if (parts.length >= 3) {
        const userId = parts[1];
        const room = parts[2];
        
        socket.join(room);
        socket.currentRoom = room;
        
        // Store peer in Redis
        const peerData = { userId, room, socketId: socket.id };
        await pubClient.hSet(`room:${room}`, socket.id, JSON.stringify(peerData));
        
        console.log(`User ${userId} registered in room ${room}`);
        
        // Notify all clients in the room about the new peer
        socket.to(room).emit('peer-joined', { id: socket.id, userId });
      }
    } else if (message.startsWith('HELLO:')) {
      // Format: HELLO:target_user_id
      const targetUserId = message.substring(6);
      console.log(`HELLO from ${socket.user.userId} to ${targetUserId}`);
      
      // Find the target user in the current room
      if (socket.currentRoom) {
        const peers = await pubClient.hGetAll(`room:${socket.currentRoom}`);
        for (const [socketId, peerDataStr] of Object.entries(peers)) {
          const peerData = JSON.parse(peerDataStr);
          if (peerData.userId === targetUserId) {
            console.log(`Routing HELLO to ${targetUserId} (socket: ${socketId})`);
            io.to(socketId).emit('message', `HELLO:${targetUserId}`);
            break;
          }
        }
      }
    } else if (message === 'WELCOME') {
      console.log(`WELCOME from ${socket.user.userId}`);
      // Route WELCOME back to the caller
      if (socket.currentRoom) {
        socket.to(socket.currentRoom).emit('message', 'WELCOME');
      }
    } else if (message === 'INIT') {
      console.log(`INIT from ${socket.user.userId}`);
      // Route INIT to the callee
      if (socket.currentRoom) {
        socket.to(socket.currentRoom).emit('message', 'INIT');
      }
    } else if (message === 'WAITING') {
      console.log(`WAITING from ${socket.user.userId}`);
      // Route WAITING back to the caller
      if (socket.currentRoom) {
        socket.to(socket.currentRoom).emit('message', 'WAITING');
      }
    } else if (message.startsWith('OFFER:')) {
      console.log(`OFFER from ${socket.user.userId}`);
      // Route OFFER to other peers in room
      if (socket.currentRoom) {
        socket.to(socket.currentRoom).emit('message', message);
      }
    } else if (message.startsWith('ANSWER:')) {
      console.log(`ANSWER from ${socket.user.userId}`);
      // Route ANSWER to other peers in room
      if (socket.currentRoom) {
        socket.to(socket.currentRoom).emit('message', message);
      }
    } else if (message.startsWith('ICE:')) {
      console.log(`ICE candidate from ${socket.user.userId}`);
      // Route ICE candidate to other peers in room
      if (socket.currentRoom) {
        socket.to(socket.currentRoom).emit('message', message);
      }
    } else if (message === 'BYE' || message === 'CANCEL') {
      console.log(`${message} from ${socket.user.userId}`);
      // Route BYE/CANCEL to other peers in room
      if (socket.currentRoom) {
        socket.to(socket.currentRoom).emit('message', message);
      }
    } else if (message === 'USER_LIST') {
      console.log(`USER_LIST request from ${socket.user.userId}`);
      if (socket.currentRoom) {
        const peers = await pubClient.hGetAll(`room:${socket.currentRoom}`);
        const uniq = [...new Set(Object.values(peers).map(p => JSON.parse(p).userId))];
        const userList = uniq.filter(u => u !== socket.user.userId).join(',');
        socket.emit('message', `USER_LIST:${userList}`);
      }
    } else {
      console.log(`Unknown protocol message from ${socket.user.userId}: ${message}`);
    }
  });

  // Handle peer registration
  socket.on('register', async (data) => {
    const { room } = data;
    const userId = socket.user.userId; // From JWT
    socket.join(room);
    socket.currentRoom = room;

    // Store peer in Redis
    const peerData = { userId, room, socketId: socket.id };
    await pubClient.hSet(`room:${room}`, socket.id, JSON.stringify(peerData));

    // Notify all clients in the room about the new peer
    socket.to(room).emit('peer-joined', { id: socket.id, userId });

    // Send current peer list to the new client
    const peers = await pubClient.hGetAll(`room:${room}`);
    socket.emit('peer-list', Object.values(peers).map(p => JSON.parse(p)));

    // Notify all clients in the room to update their user list
    io.to(room).emit('update-user-list');
  });

  // Handle WebRTC signaling
  socket.on('offer', (data) => {
    socket.to(data.target).emit('offer', { sdp: data.sdp, sender: socket.id });
  });
  socket.on('answer', (data) => {
    socket.to(data.target).emit('answer', { sdp: data.sdp, sender: socket.id });
  });
  socket.on('ice-candidate', (data) => {
    socket.to(data.target).emit('ice-candidate', { candidate: data.candidate, sender: socket.id });
  });

  // Handle disconnection
  socket.on('disconnect', async () => {
    const rooms = Array.from(socket.rooms).filter(r => r !== socket.id);
    for (const room of rooms) {
      const peer = await pubClient.hGet(`room:${room}`, socket.id);
      if (peer) {
        const peerData = JSON.parse(peer);
        socket.to(room).emit('peer-left', { id: socket.id, userId: peerData.userId });
        await pubClient.hDel(`room:${room}`, socket.id);
        // Notify all clients in the room to update their user list
        io.to(room).emit('update-user-list');
      }
    }
    console.log('Client disconnected:', socket.id);
  });
});

// Endpoint to generate JWT (for testing or client authentication)
app.use(express.json());
app.post('/login', (req, res) => {
  const { userId, password } = req.body;
  // Replace with proper authentication (e.g., database lookup)
  if (password === 'valid-password') {
    const token = jwt.sign({ userId }, JWT_SECRET, { expiresIn: '1h' });
    res.json({ token });
  } else {
    res.status(401).json({ error: 'Invalid credentials' });
  }
});

// Endpoint to get list of registered users with pagination
app.get('/users/:room', async (req, res) => {
  const room = req.params.room;
  const page = parseInt(req.query.page) || 1;
  const limit = parseInt(req.query.limit) || 10;
  const offset = (page - 1) * limit;

  try {
    const peers = await pubClient.hGetAll(`room:${room}`);
    const peerList = Object.values(peers).map(p => JSON.parse(p));
    const total = peerList.length;
    const paginatedList = peerList.slice(offset, offset + limit);

    res.json({
      page,
      limit,
      total,
      pages: Math.ceil(total / limit),
      users: paginatedList
    });
  } catch (error) {
    console.error('Error fetching user list:', error);
    res.status(500).json({ error: 'Failed to fetch user list' });
  }
});

console.log('Starting server on port ' + PORT + '...');
server.listen(PORT, () => {
  console.log('Server running on port ' + PORT);
  console.log('Server successfully started and listening');
});