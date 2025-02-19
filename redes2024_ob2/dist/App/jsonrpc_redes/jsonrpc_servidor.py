import inspect
import socket
import json
import threading

class Server:
    
    def __init__(self, server_address):
        self.address, self.port = server_address  
        self.methods = {}
        self.running = False 
        self.server_socket = None
        self.threads = []  

    def add_method(self, method, name=None):
        method_name = name if name else method.__name__
        self.methods[method_name] = method

    def serve(self):
        # Crear el socket del servidor
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.bind((self.address, self.port))
        self.server_socket.listen()
        self.running = True
        print(f"Servidor escuchando en {self.address}:{self.port}...")
        
        while self.running:
            try:
                self.server_socket.settimeout(1)
                client_socket, client_address = self.server_socket.accept()
                print(f"Conexión desde {client_address} aceptada.")
                thread = threading.Thread(target=self.handle_client, args=(client_socket,))
                thread.start()
                self.threads.append(thread) 
            except socket.timeout:
                continue
            except Exception as e:
                print(f"Error en el servidor: {e}")
                break

    def handle_client(self, client_socket):
        with client_socket:
            while True:
                try:
                    # Recibir datos del cliente
                    data = b""
                    while True:
                        part = client_socket.recv(1024)
                        data += part
                        # Verifica si el mensaje está completo (JSON debe estar bien formado)
                        #print('data: ', data)
                        try:
                            request = json.loads(data.decode())
                            break  # Si el JSON es válido, salir del bucle
                        except json.JSONDecodeError as e:
                            if not part:
                                return self.jsonrpc_error('-32700','Parse error')
                    if not data:
                        break  # Si no hay datos, salir del bucle
                    
                    # Procesar la solicitud y obtener la respuesta
                    response = self.handle_request(request)
                    
                    if response:
                        # Enviar la respuesta al cliente
                        response_data = json.dumps(response).encode()
                        total_sent = 0
                        while total_sent < len(response_data):
                            sent = client_socket.send(response_data[total_sent:])
                            if sent == 0:
                                raise RuntimeError("Conexión interrumpida durante el envío")
                            total_sent += sent
                except Exception as e:
                    print(f"Error al manejar la solicitud: {e}")
                    break

    def handle_request(self, request):
        method_name = request.get('method')
        params = request.get('params', [])
        request_id = request.get('id')

        try:
            # 1. Verificar que el campo 'jsonrpc' esté presente y sea "2.0"
            if request.get('jsonrpc') != '2.0':
                return self.jsonrpc_error('-32600', 'Invalid Request', None)
        
            # 2. Verificar que 'method' esté presente y sea un string
            if not isinstance(method_name, str):
                return self.jsonrpc_error('-32600', 'Invalid Request', None)
        
            # 3. Verificar que 'id' (si está presente) sea un número, string o None
            request_id = request.get('id')
            if request_id is not None and not isinstance(request_id, (int, str)):
                return self.jsonrpc_error('-32600', 'Invalid Request', None)
        
            # 4. Verificar que 'params' sea una lista o un diccionario (opcional)
            params = request.get('params', [])
            if not isinstance(params, (list, dict)):
                return self.jsonrpc_error('-32600', 'Invalid Request: params must be a list or object', request_id)

            if method_name not in self.methods:
                return self.jsonrpc_error('-32601','Method not found', request_id)
            
        except Exception as e:
        # Aquí capturas cualquier error interno y lanzas un error de servidor
            return self.jsonrpc_error('-32000', f'Server error: {str(e)}', request_id)
        
        #Verificamos que los parámetros de params sean correctos
        method = self.methods[method_name]

        #firma de la funcion
        firma = inspect.signature(method)

        #obtenemos los parámetros requeridos según la firma
        required_params = [
            p for p in firma.parameters.values()
            if p.default == inspect.Parameter.empty and p.kind in (inspect.Parameter.POSITIONAL_OR_KEYWORD, inspect.Parameter.POSITIONAL_ONLY)
        ]

        #Si la función tiene parámetros obligatorios entonces required_params tiene esos parámetros
        #Si la función tiene parámetros opcionaes entonces required_params = 0
        #Python entiende que una función tiene parámetros opcionales si al momento de definir la función dichos parámetros tienen un valor inical (ej: def metodoConParametrosOpcionales(param1=None, param2=None):

        total_params = len(firma.parameters)

        #total_params es la cantidad de parámetros de la firma de la función, obligatorios + opcionales

        # 1. Verificar si la función no acepta ningún parámetro
        if not total_params and params:
            return self.jsonrpc_error('-32602', 'Invalid params', request_id)

        # 2. Verificar si faltan parámetros para las funciones con parámetros obligatorios
        if len(params) < len(required_params):
            return self.jsonrpc_error('-32602', 'Invalid params', request_id)

        # 3. Verificar si hay demasiados parámetros, solo para funciones con parámetros obligatorios
        if not any(p.kind == inspect.Parameter.VAR_POSITIONAL for p in firma.parameters.values()):
            if len(params) > total_params:
                return self.jsonrpc_error('-32602', 'Invalid params', request_id)
            
        try:
            if isinstance(params, dict):
                params = list(params.values())
            result = method(*params)
            return {'jsonrpc': '2.0', 'result': result, 'id': request_id}
        except Exception as e:
            return self.jsonrpc_error('-32603', 'Internal error', request_id, str(e))

    def jsonrpc_error(self, code, message, request_id = None, data=None):
        error_object = {
            "jsonrpc": "2.0",
            "error": {
                "code": code,
                "message": message
            },
            "id": request_id
        }
        if data:
            error_object["error"]["data"] = data
        return error_object

    def shutdown(self):
        self.running = False
        if self.server_socket:
            self.server_socket.close()
        for thread in self.threads:
            thread.join() 
        print("Servidor cerrado.")