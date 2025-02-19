# ------------------------------------------------------
# Descripción: 
# Iimplementamos la lógica del cliente que se conecta a dos servidores 
# remotos usando la biblioteca jsonrpc_redes. El cliente llama a procedimientos remotos (RPC).

# Los servidores ofrecen procedimientos que son invocados a través 
# de la conexión establecida con cada uno.
# ------------------------------------------------------
import sys
import threading

from jsonrpc_redes.jsonrpc_cliente import connect
import subprocess

def ping_host(host):
    try:
        result = subprocess.run(['ping', '-c', '4', host], capture_output=True, text=True)
        
        # Si el ping fue exitoso
        if result.returncode == 0:
            print(f"Ping a {host} exitoso")
            print(result.stdout)
        else:
            print(f"Falló el ping a {host}")
            print(result.stderr)
    except Exception as e:
        print(f"Error al intentar hacer ping: {e}")


def traceroute_host(host):
    try:
        result = subprocess.run(['traceroute', '-n', host], capture_output=True, text=True)
        
        # Si el traceroute fue exitoso
        if result.returncode == 0:
            print(f"Traceroute a {host} exitoso")
            print(result.stdout)  
        else:
            print(f"Falló el traceroute a {host}")
            print(result.stderr)  
    except Exception as e:
        print(f"Error al intentar hacer traceroute: {e}")

ping_host("200.0.0.10")
traceroute_host("200.0.0.10")

#TEST CONEXIONES MULTIPLES
def conexion(ip, puerto):
    # Conectar al servidor
    conn = connect(ip, puerto)
    a = 0 #Control de errores
    # Verificación de la conexión
    if conn is None:
        print(f"No se pudo establecer la conexión con el servidor {ip}:{puerto}.")
    else:
        try:
            # llamadas a procedimientos remotos (RPC)
            result = conn.add(2, 2) 
            print(f"[{ip}:{puerto}] Resultado de add: {result}")

            result = conn.multiply(8, 8)
            print(f"[{ip}:{puerto}] Resultado de multiply: {result}")
        except Exception as e:
            a = 1 #Control de errores
            print(f"Error en la conexión {ip}:{puerto}: {str(e)}")
        
        finally:
            # Cerrar la conexión
            if conn is not None:
                conn.close()
                if a == 0:
                    print('Conexiones multiples exitosa')
                print(f"Conexión con {ip}:{puerto} cerrada.")

# Bloque principal del programa
if __name__ == "__main__":
    #Conexiones a diferentes servidores utilizando la función connect
    conn = connect('200.0.0.10', 5000)
    conn1 = connect('200.100.0.15', 1234)
    conn2 = connect('200.0.0.10', 5000)
    #conn = connect('localhost', 5000)
    #conn1 = connect('localhost', 1234)
    #conn2 = connect('localhost', 1234)

    # Verificación de la conexión con el servidor 1
    if conn is None:
        print("No se pudo establecer la conexión con el servidor 1.")
    else:
        try:
            # Llamadas a procedimientos remotos en el servidor 1
            result = conn.add(5, 10) 
            print("Resultado de add:", result)

            result = conn.mix(5, param1 = 'Hola')
            print('resultado de mix:', result)

            result = conn.substract(5, 10)
            print("Resultado de substract:", result)

            result = conn.multiply(5, 10)
            print("Resultado de multiply:", result)

            result = conn.sinParams()
            print("Resultado de sinParams:", result)

            result1, result2 = conn.dosValores(10,12)
            print("Resultado de dosValores:", result1, result2)

            result = conn.metodoConParametrosOpcionales(param1='opcional1', param2=2)
            print("Resultado de metodoConParametrosOpcionales:", result)

            result = conn.metodoConParametrosOpcionales()
            print("Resultado de metodoConParametrosOpcionales:", result)

            # Envío de notificaciones al servidor (sin esperar una respuesta)
            conn.notificacionSinParametros(notify=True)
            conn.notificacionConParametros(notify=True)
            conn.notificacionConParametros(param1='opcional1', notify=True)

            # Pruebas de manejo de errores
            try:
                conn.add(3)
            except Exception as e:
                print('Llamada con cantidad incorrecta de parámetros. Genera excepción necesaria.')
                print(e.code, e.message)
            else:
                print('ERROR: No lanzó excepción.')

            try:
                conn.add(3, 2, 1)
            except Exception as e:
                print('Llamada con cantidad incorrecta de parámetros. Genera excepción necesaria.')
                print(e.code, e.message)
            else:
                print('ERROR: No lanzó excepción.')

            try:
                conn.metNoExiste(5, 6)
            except Exception as e:
                print('Llamada a método inexistente. Genera excepción necesaria.')
                print(e.code, e.message)
            else:
                print('ERROR: No lanzó excepción.')

        finally:
            # Cierra la conexión con el servidor 1
            if conn is not None:
                conn.close()

    # Verificación de la conexión con el servidor 2
    if conn1 is None:
        print("No se pudo establecer la conexión con el servidor 2.")
    else:
        try:
            # Llamadas a procedimientos remotos en el servidor 2
            result = conn1.divide(10, 2)
            print("Resultado de divide:", result)

            result = conn1.power(10, 2)
            print("Resultado de power:", result)

            result = conn1.concat_strings('amamos ', 'redes')
            print("Resultado de concat_strings:", result)

        finally:
            # Cierra la conexión con el servidor 2
           if conn1 is not None:
              conn1.close()

    # Verificación de la segunda conexión con el servidor 1
    if conn2 is None:
        print("No se pudo establecer la conexión con el servidor 1.")
    else:
        try:
            # Realiza una nueva operación de suma
            result = conn.add(20, 13) 
            print("Resultado de add:", result)

        finally:
            # Cierra la segunda conexión con el servidor 1
            if conn2 is not None:
                conn2.close()

    #thread1 = threading.Thread(target=conexion, args=('localhost', 5000))
    #thread1.start()
    #thread2 = threading.Thread(target=conexion, args=('localhost', 5000))
    #thread2.start()
    #thread3 = threading.Thread(target=conexion, args=('localhost', 5000))
    #thread3.start()

    thread1 = threading.Thread(target=conexion, args=('200.0.0.10', 5000))
    thread1.start()
    thread2 = threading.Thread(target=conexion, args=('200.0.0.10', 5000))
    thread2.start()
    thread3 = threading.Thread(target=conexion, args=('200.0.0.10', 5000))
    thread3.start()
