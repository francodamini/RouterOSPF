import json
import socket

# Definición de la clase Cliente
class Cliente:

    def __init__(self, address, port):
        """
        Inicializa una nueva instancia de Cliente.
        
        Parámetros:
        - address: Dirección IP o nombre de host del servidor.
        - port: Número de puerto del servidor.
        """
        self.id_counter = 1
        self.address = address
        self.port = port
        self.master = None

    def _send_request(self, method, params=None, notify=False):
        """
        Envía una solicitud al servidor.

        Parámetros:
        - method: Nombre del procedimiento remoto a ejecutar.
        - params: Parámetros a enviar al procedimiento (parámetro opcional).
        - notify: Si es True, se envía como notificación y no se espera respuesta.

        Retorna:
        - El resultado del procedimiento remoto si no es una notificación.
        
        Lanza:
        - JsonRpcError si ocurre un error en la ejecución.
        - ConnectionError si hay problemas al conectar con el servidor.
        """

        master = socket.socket(socket.AF_INET, socket.SOCK_STREAM)  # Crear un nuevo socket TCP
        a = 0  # Indicador de control para manejo de errores
        try:
            master.connect((self.address, self.port))  # Intentar conectar al servidor
            self.master = master

            # Construir la solicitud en formato JSON-RPC 2.0
            request = {
                "jsonrpc": "2.0",
                "method": method,
                "params": params,
                "id": self.id_counter if not notify else None
            }
            self.id_counter += 1 # Incrementar el ID para futuras solicitudes
            
            if params == 'error':
                a = 1  # Indicador de control para manejo de errores
                raise JsonRpcError('-32603', "Internal error")

            # Convertir la solicitud a formato JSON y enviarla al servidor
            # data = json.dumps(request).encode('utf-8')
            data = json.dumps(request).encode()
            total_sent = 0
            while total_sent < len(data):
                sent = master.send(data[total_sent:])
                if sent == 0:
                    raise RuntimeError("Conexión interrumpida durante el envío")
                total_sent += sent

            #remain = master.send(data) # Enviar datos por el socket
            if sent is None:
                print(f"Error al enviar datos:")
                return None
            
            # Si no es notificación, esperar la respuesta del servidor
            if not notify:
                response = master.recv(1024) # Leer respuesta del servidor
                if response is None: 
                    if not response:
                        raise RuntimeError("Error al recibir respuesta del servidor.")
                res = json.loads(response) # Parsear la respuesta del servidor

                # Manejo de errores en la respuesta
                if 'error' in res:
                    a = 1  # Indicador de control para manejo de errores
                    error = res['error']
                    raise JsonRpcError(error['code'], error['message']) # Lanza excepción si hay error
                
                # Verificar que el ID de la respuesta coincida con el de la solicitud
                if res['id'] != request['id']:
                    raise RuntimeError(f"ID de la respuesta ({res['id']}) no coincide con el de la solicitud ({request['id']}).")

                return res['result'] # Retorna el resultado si no hay error
            
        # Manejo de excepciones específicas de JSON-RPC
        except JsonRpcError as e:
            if a == 1:
                raise JsonRpcError(e.code, e.message)
            if a == 0:
                raise ConnectionError(f"Error de conexión al servidor en {self.address}:{self.port} - {e}")  # Lanza excepción de conexión

    def __getattr__(self, name):
        """
        Se llama cuando una búsqueda de atributo no ha encontrado el atributo en los lugares habituales.

        Parámetros:
        - name: Nombre del método que se intenta llamar.
        
        Retorna:
        - Una función que envía la solicitud correspondiente al método remoto.
        """
        def method(*args, **kwargs):
            notify = kwargs.pop('notify', False) # Extrae si es notificación

            if args and kwargs:
                # Combina los argumentos posicionales y los de palabra clave en un solo diccionario
                params = {f'arg{i}': arg for i, arg in enumerate(args)}
                params.update(kwargs)  # Añadir kwargs al diccionario de params
            # Si se usan argumentos con nombre (kwargs), los enviamos como un diccionario.
            elif kwargs:
                params = kwargs
            # Si se usan argumentos posicionales, enviamos una lista.
            else:
                params = args
            return self._send_request(name, params, notify=notify)
        return method

    def close(self):
        if self.master:
            self.master.close()

def connect(address, port):
        cliente = Cliente(address, port)
        return cliente


class JsonRpcError(Exception):
    def __init__(self, code, message):
        super().__init__(f"Error {code}: {message}")  # Esto genera un mensaje para la excepción base
        self.code = code
        self.message = message
