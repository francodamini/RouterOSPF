import threading
import time
import sys
from jsonrpc_redes import Server

def server2():
    # Procedimientos del servidor 2
    def divide(x, y):
        if y == 0:
            raise ValueError("Cannot divide by zero")
        return x / y

    def power(base, exponent):
        return base ** exponent

    def concat_strings(s1, s2):
        return s1 + s2

# Crear el servidor en la dirección y puerto deseados
    server = Server(('', 1234))
    #server = Server(('localhost', 1234))
    # Registrar los métodos que estarán disponibles para los clientes
    server.add_method(divide)
    server.add_method(power)
    server.add_method(concat_strings)
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
    server2()
