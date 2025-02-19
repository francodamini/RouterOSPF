
import threading
import time
import sys
from jsonrpc_redes import Server

def server1():
    # Procedimientos del servidor 1
    def add(x, y):
        return x + y

    def mix(x, param1=None):
        return x, param1

    def substract(x, y):
        return x - y

    def multiply(x, y):
        return x * y

    def sinParams():
        return "METODO SIN PARAMETROS"

    def dosValores(x, y):
        return x, y

    def metodoConParametrosOpcionales(param1=None, param2=None):
        return {"param1": param1, "param2": param2}

    def notificacionSinParametros():
        print("Notificación sin parámetros recibida.")

    def notificacionConParametros(param1=None, param2=None):
        print(f"Notificación con parámetros recibida: param1={param1}, param2={param2}")


    # Configuración y ejecución del servidor
    # Crear el servidor en la dirección y puerto deseados
    server = Server(('', 5000))
    #server = Server(('localhost', 5000))
    
    # Registrar los métodos que estarán disponibles para los clientes
    server.add_method(add)
    server.add_method(mix)
    server.add_method(substract)
    server.add_method(multiply)
    server.add_method(sinParams)
    server.add_method(dosValores)
    server.add_method(metodoConParametrosOpcionales)
    server.add_method(notificacionSinParametros)
    server.add_method(notificacionConParametros)
    server_thread = threading.Thread(target=server.serve)
    server_thread.daemon = True
    server_thread.start()
    # Iniciar el servidor para que empiece a escuchar y manejar solicitudes
    print("Servidor RPC iniciado. Esperando conexiones...")

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        server.shutdown()
        print('Terminado.')
        sys.exit()

if __name__ == "__main__":
    server1()
