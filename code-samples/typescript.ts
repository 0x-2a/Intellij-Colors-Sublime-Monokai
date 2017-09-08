/*
    Sample from https://angular.io/guide/http
 */

@Injectable()
export class CachingInterceptor implements HttpInterceptor {
    private HttpResponse: any;

    constructor(private cache: HttpCache) {
    }

    intercept(req: HttpRequest<any>, next: HttpHandler): Observable<HttpEvent<any>> {
        // Before doing anything, it's important to only cache GET requests.
        // Skip this interceptor if the request method isn't GET.
        if (req.method !== 'GET') {
            return next.handle(req);
        }

        // First, check the cache to see if this request exists.
        const cachedResponse = this.cache.get(req);
        if (cachedResponse) {
            // A cached response exists. Serve it instead of forwarding
            // the request to the next handler.
            return Observable.of(cachedResponse);
        }

        // No cached response exists. Go to the network, and cache
        // the response when it arrives.
        return next.handle(req).do(event => {
            // Remember, there may be other events besides just the response.
            if (event instanceof this.HttpResponse) {
                // Update the cache.
                this.cache.put(req, event);
            }
        });
    }
}

function Injectable() {
}

interface HttpInterceptor {
}

interface HttpCache {
    get(req: HttpRequest<any>): any;

    put(req: HttpRequest<any>, event: any): any;
}

class HttpRequest<T> {
    method: string;
}

class HttpHandler {
    handle(req: HttpRequest<any>) {
    }
}

class Observable<T> {
    static of(cachedResponse: any) {
    }
}

class HttpEvent<T> {
}